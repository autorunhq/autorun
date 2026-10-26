/* Folding the log's floods: the same line a hundred times over says nothing
 * its first two did not.  Lines of the kinds that repeat by the hundred (the
 * loader walking a DLL it has seen, faults a program polls on, a map that
 * keeps hitting pages already taken) are counted and summarised instead of
 * written; everything else is logged as it comes.
 *
 * A shape keeps what makes two lines the same kind of event: the [EXC] faults
 * keep their code, address and pc, so one storm of one fault folds while a
 * different fault beside it does not; the other lines are their text with the
 * numbers masked, so the twentieth avrt.dll load folds and the loads of the
 * DLLs beside it do not.  The first two lines of a shape are written as they
 * are, from the third one on a summary goes out at most every half minute
 * ("xN more like: the first of them"), and a shape left alone for that long
 * starts over.
 *
 * Pure policy, no side effects: the runtime drives it from its log_line with
 * its own clock and lock (horizon-wine/source/runtime.c), and the host test
 * walks the same calls (horizon-wine/tests/log_repeat.c).
 */
#ifndef AUTORUN_LOG_REPEAT_H
#define AUTORUN_LOG_REPEAT_H

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LOG_REPEAT_SHAPE 96
#define LOG_REPEAT_SAMPLE 160
#define LOG_REPEAT_SLOTS 48
#define LOG_REPEAT_KEEP 2
#define LOG_REPEAT_WINDOW_MS 30000ull

struct log_repeat_entry
{
    char shape[LOG_REPEAT_SHAPE];
    char sample[LOG_REPEAT_SAMPLE];   /* the first line of the shape */
    unsigned long long seen_ms;
    unsigned long long summary_ms;
    unsigned int printed;             /* full lines written, up to LOG_REPEAT_KEEP */
    unsigned int since_summary;       /* lines folded since the last summary */
};

struct log_repeat_table
{
    struct log_repeat_entry entries[LOG_REPEAT_SLOTS];
};

enum log_repeat_action
{
    LOG_REPEAT_PRINT,     /* write the line */
    LOG_REPEAT_SUPPRESS,  /* count it for a summary, write nothing */
    LOG_REPEAT_SUMMARY,   /* write out instead of the line */
};

/* The kinds of line that fold.  Everything else keeps its every line. */
static inline int log_repeat_wanted( const char *line )
{
    static const char *const tags[] =
    {
        "[EXC]", "[HMAP]", "[NXLDR]", "[NXDELAY]", "[TR]", "[LDR]", "[ATTACH]", "[WOW64]", "[WOW64 THREAD]",
    };
    unsigned int i;

    for (i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (!strncmp( line, tags[i], strlen( tags[i] ) )) return 1;
    /* Wine's own channels, in the log as "0004:err:module: ..." */
    if (isdigit( (unsigned char)line[0] ))
    {
        const char *colon = strchr( line, ':' );

        if (colon && (!strncmp( colon, ":err:", 5 ) || !strncmp( colon, ":warn:", 6 ) ||
                      !strncmp( colon, ":fixme:", 7 )))
            return 1;
    }
    return 0;
}

/* What two lines must share to be the same event.  An [EXC] line keeps its
 * code, faulting address and pc (between "code=" and " sp="); the rest lose
 * their numbers, which are what differs between repeats. */
static inline void log_repeat_shape( const char *line, char *shape, size_t size )
{
    size_t o = 0;

    if (!strncmp( line, "[EXC] fault ", 12 ))
    {
        const char *code = strstr( line, "code=" ), *sp = strstr( line, " sp=" );

        if (code && sp && sp > code && (size_t)(sp - code) + 6 < size)
        {
            snprintf( shape, size, "[EXC] %.*s", (int)(sp - code), code );
            return;
        }
    }
    while (*line && o + 2 < size)
    {
        if (line[0] == '0' && (line[1] == 'x' || line[1] == 'X') && isxdigit( (unsigned char)line[2] ))
        {
            line += 2;
            while (isxdigit( (unsigned char)*line )) line++;
            shape[o++] = '#';
        }
        else if (isdigit( (unsigned char)*line ))
        {
            while (isdigit( (unsigned char)*line )) line++;
            shape[o++] = '#';
        }
        else shape[o++] = *line++;
    }
    shape[o] = 0;
}

/* PRINT: write line.  SUPPRESS: write nothing.  SUMMARY: write out (already
 * formatted) instead of the line; it stands for the folded ones, the line
 * among them. */
static inline enum log_repeat_action log_repeat_next( struct log_repeat_table *table, unsigned long long now_ms,
                                                      const char *line, char *out, size_t out_size )
{
    char shape[LOG_REPEAT_SHAPE];
    struct log_repeat_entry *e = NULL, *oldest = table->entries;
    unsigned int i;

    if (!log_repeat_wanted( line )) return LOG_REPEAT_PRINT;
    log_repeat_shape( line, shape, sizeof(shape) );
    for (i = 0; i < LOG_REPEAT_SLOTS; i++)
    {
        struct log_repeat_entry *slot = &table->entries[i];

        if (slot->shape[0] && !strcmp( slot->shape, shape )) { e = slot; break; }
        if (!slot->shape[0] || slot->seen_ms < oldest->seen_ms) oldest = slot;
    }
    if (!e)
    {
        /* A shape not seen yet takes its place; the oldest one may go with a
         * summary still owed, which this return has no room to carry. */
        e = oldest;
        memset( e, 0, sizeof(*e) );
        snprintf( e->shape, sizeof(e->shape), "%s", shape );
        snprintf( e->sample, sizeof(e->sample), "%s", line );
        e->printed = 1;
        e->seen_ms = e->summary_ms = now_ms;
        return LOG_REPEAT_PRINT;
    }
    /* A shape that has been quiet for a whole window is a new event again. */
    if (now_ms - e->seen_ms > LOG_REPEAT_WINDOW_MS && !e->since_summary)
    {
        e->printed = 1;
        e->seen_ms = e->summary_ms = now_ms;
        return LOG_REPEAT_PRINT;
    }
    e->seen_ms = now_ms;
    if (e->printed < LOG_REPEAT_KEEP)
    {
        e->printed++;
        e->summary_ms = now_ms;
        e->since_summary = 0;
        return LOG_REPEAT_PRINT;
    }
    e->since_summary++;
    if (now_ms - e->summary_ms >= LOG_REPEAT_WINDOW_MS)
    {
        if (out && out_size) snprintf( out, out_size, "[LOG] x%u more like: %s", e->since_summary, e->sample );
        e->summary_ms = now_ms;
        e->since_summary = 0;
        return LOG_REPEAT_SUMMARY;
    }
    return LOG_REPEAT_SUPPRESS;
}

/* The summaries still owed, one per call, for the way out. */
static inline int log_repeat_flush( struct log_repeat_table *table, char *out, size_t out_size )
{
    unsigned int i;

    for (i = 0; i < LOG_REPEAT_SLOTS; i++)
    {
        struct log_repeat_entry *e = &table->entries[i];

        if (!e->shape[0] || !e->since_summary) continue;
        if (out && out_size) snprintf( out, out_size, "[LOG] x%u more like: %s", e->since_summary, e->sample );
        e->since_summary = 0;
        return 1;
    }
    return 0;
}

#endif
