/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Written by horizon-wine/tools/make-directshow-reg.py; change that, not this.
 *
 * The DirectShow splitter of winegstreamer.dll, which plays a file through
 * the runtime's FFmpeg parser and which no DllRegisterServer registers here,
 * with the categories and the quartz.dll filters a filter graph connects it
 * to. The same keys Carafe adds to classes.reg. Loaded into
 * \Registry\Machine by horizon_registry_init, before system.reg. */
#ifndef __WINE_NX_HORIZON_DIRECTSHOW_REG_H
#define __WINE_NX_HORIZON_DIRECTSHOW_REG_H

static const char horizon_directshow_seed[] =
    "WINE REGISTRY Version 2\n"
    ";; DirectShow: the categories and filters of make-directshow-reg.py\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{DA4E3DA0-D07D-11D0-BD50-00A0C911CE86}\\\\Instance\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}]\n"
    "\"CLSID\"=\"{083863F1-70DE-11D0-BD40-00A0C911CE86}\"\n"
    "\"FriendlyName\"=\"DirectShow Filters\"\n"
    "\"Merit\"=dword:00600000\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{DA4E3DA0-D07D-11D0-BD50-00A0C911CE86}\\\\Instance\\\\{E0F158E1-CB04-11D0-BD4E-00A0C911CE86}]\n"
    "\"CLSID\"=\"{E0F158E1-CB04-11D0-BD4E-00A0C911CE86}\"\n"
    "\"FriendlyName\"=\"Audio Renderers\"\n"
    "\"Merit\"=dword:00600000\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{F9D8D64E-A144-47DC-8EE0-F53498372C29}]\n"
    "\"CLSID\"=\"{F9D8D64E-A144-47DC-8EE0-F53498372C29}\"\n"
    "\"FriendlyName\"=\"GStreamer splitter filter\"\n"
    "\"FilterData\"=hex:02,00,00,00,ff,ff,5f,00,03,00,00,00,00,00,00,00,30,70,69,33,00,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,88,00,00,00,98,00,00,00,31,70,69,33,08,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,a8,00,00,00,98,00,00,00,32,70,69,33,08,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,b8,00,00,00,98,00,00,00,83,eb,36,e4,4f,52,ce,11,9f,53,00,20,af,0b,a7,70,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,61,75,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71,76,69,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{70E102B0-5556-11CE-97C0-00AA0055595A}]\n"
    "\"CLSID\"=\"{70E102B0-5556-11CE-97C0-00AA0055595A}\"\n"
    "\"FriendlyName\"=\"Video Renderer\"\n"
    "\"FilterData\"=hex:02,00,00,00,00,00,80,00,01,00,00,00,00,00,00,00,30,70,69,33,02,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,38,00,00,00,48,00,00,00,76,69,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{6BC1CFFA-8FC1-4261-AC22-CFB4CC38DB50}]\n"
    "\"CLSID\"=\"{6BC1CFFA-8FC1-4261-AC22-CFB4CC38DB50}\"\n"
    "\"FriendlyName\"=\"Video Renderer\"\n"
    "\"FilterData\"=hex:02,00,00,00,01,00,80,00,01,00,00,00,00,00,00,00,30,70,69,33,02,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,38,00,00,00,48,00,00,00,76,69,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{E436EBB5-524F-11CE-9F53-0020AF0BA770}]\n"
    "\"CLSID\"=\"{E436EBB5-524F-11CE-9F53-0020AF0BA770}\"\n"
    "\"FriendlyName\"=\"File Source (Async.)\"\n"
    "\"FilterData\"=hex:02,00,00,00,00,00,40,00,01,00,00,00,00,00,00,00,30,70,69,33,08,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,38,00,00,00,48,00,00,00,83,eb,36,e4,4f,52,ce,11,9f,53,00,20,af,0b,a7,70,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{6A08CF80-0E18-11CF-A24D-0020AFD79767}]\n"
    "\"CLSID\"=\"{6A08CF80-0E18-11CF-A24D-0020AFD79767}\"\n"
    "\"FriendlyName\"=\"ACM Wrapper\"\n"
    "\"FilterData\"=hex:02,00,00,00,f0,ff,5f,00,02,00,00,00,00,00,00,00,30,70,69,33,00,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,60,00,00,00,70,00,00,00,31,70,69,33,08,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,60,00,00,00,70,00,00,00,61,75,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
    "\n"
    "[Software\\\\Classes\\\\CLSID\\\\{083863F1-70DE-11D0-BD40-00A0C911CE86}\\\\Instance\\\\{CF49D4E0-1115-11CE-B03A-0020AF0BA770}]\n"
    "\"CLSID\"=\"{CF49D4E0-1115-11CE-B03A-0020AF0BA770}\"\n"
    "\"FriendlyName\"=\"AVI Decompressor\"\n"
    "\"FilterData\"=hex:02,00,00,00,f0,ff,5f,00,02,00,00,00,00,00,00,00,30,70,69,33,00,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,60,00,00,00,70,00,00,00,31,70,69,33,08,00,00,00,00,00,00,00,01,00,00,00,00,00,00,00,00,00,00,00,30,74,79,33,00,00,00,00,60,00,00,00,70,00,00,00,76,69,64,73,00,00,10,00,80,00,00,aa,00,38,9b,71,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
    "\n";

#endif
