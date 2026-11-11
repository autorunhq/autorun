#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <switch.h>

#include "carafe_draw.h"
#include "carafe_error.h"
#include "carafe_menu.h"
#include "carafe_overlay.h"
#include "carafe_registry.h"
#include "carafe_settings.h"
#include "osk.h"

#define PENDING_ERROR_FILE "sdmc:/switch/wine/carafe/pending-error.txt"
#define RUN_NEXT_FILE "sdmc:/switch/wine/run-next.txt"
#define PATH_SIZE 768
#define ERROR_FLAG "--carafe-error"
#define RUNTIME_NRO "sdmc:/switch/wine/wine-nx-runtime.nro"
#define SCREEN_WIDTH CARAFE_SCREEN_WIDTH
#define SCREEN_HEIGHT CARAFE_SCREEN_HEIGHT
#define ARGV_SIZE 2048
#define NAME_SIZE 96
#define LINE_SIZE 160
#define MAX_BUTTONS 3
#define MAX_ROWS 4
#define CHANGES_SIZE (64 * 1024)

#define MARGIN 72
#define TEXT_WIDTH 600
#define ICON_SIZE 168
#define ICON_RADIUS 32
#define BUTTON_HEIGHT 64
#define BUTTON_RADIUS 16
#define BUTTON_GAP 16
#define BUTTON_PAD 30
#define BUTTON_FOCUS_PAD 14
#define GLYPH_SIZE 34
#define CARD_WIDTH 440
#define CARD_X (SCREEN_WIDTH - MARGIN - CARD_WIDTH)
#define CARD_RADIUS 24
#define CARD_PAD_X 36
#define CARD_PAD_Y 40

typedef enum {
    KindCrash,
    KindThreads,
    KindMemory,
    KindSetup,
    KindSystem,
    KindCount,
} Kind;

typedef enum {
    ChoiceNone,
    ChoiceRestart,
    ChoiceExit,
    ChoiceSettings,
    ChoiceRevert,
    ChoiceKeep,
    ChoiceRecommended,
} Choice;

typedef struct {
    const char* title;
    const char* body;
    const char* hint;
} KindText;

typedef struct {
    const char* fallback_title;
    const char* closed;
    const char* restart;
    const char* exit;
    const char* code;
    const char* module;
    const char* missing;
    const char* played;
    const char* settings;
    const char* revert;
    const char* keep;
    const char* recommended;
    const char* trial_chip;
    const char* again;
    KindText kinds[KindCount];
    KindText trial;
} Strings;

typedef struct {
    Kind kind;
    unsigned int code;
    unsigned int seconds;
    bool trial;
    char module[NAME_SIZE];
    char missing[NAME_SIZE];
} Report;

typedef struct {
    Choice choice;
    int x;
    int y;
    int width;
    const char* label;
} Button;

static const Strings STRINGS_EN = {
    "Windows game",
    "Closed with an error",
    "Restart game",
    "Exit",
    "Code",
    "Module",
    "Missing",
    "Played",
    "Settings",
    "Bring back",
    "Keep new",
    "Recommended",
    "Trial run failed",
    "The game has failed more than once in a row. You can return to the default settings.",
    {
        {
            "The game stopped working",
            "The game hit a problem it couldn't recover from and had to close. Anything the game saved before this "
            "is kept.",
            "Try starting it again. If it keeps happening at the same spot, look in the settings — they have tips "
            "for this error.",
        },
        {
            "The game tried to do too much at once",
            "The Switch lets a game run only a limited number of tasks at the same time. This game needed one more "
            "and had to close.",
            "Starting the game again usually helps. If it happens every time, adjusting system or thread settings may fix it.",
        },
        {
            "The game ran out of memory",
            "It asked for more memory than this game can use on the Switch and had to close.",
            "Lower the resolution or texture quality in the game's options, or look in the settings, then start it "
            "again.",
        },
        {
            "Some of the game's files are missing",
            "The game needs a Windows file that isn't present, so it can't start.",
            "Check that the game folder is complete and that Autorun's Windows DLLs are installed in Settings > System.",
        },
        {
            "The Windows environment stopped",
            "The runtime that executes Windows games on the Switch hit an error, and the game had to close.",
            "Start the game again. If it happens every time, check Settings or try another CPU/D3D option.",
        },
    },
    {
        "The game crashed with the new settings",
        "This was a trial run: the game closed with an error after its settings were changed. The settings it ran "
        "with before are kept.",
        "Bring them back to start the game as it worked before the change, or keep the new ones if the error does "
        "not seem to be theirs.",
    },
};

static const Strings STRINGS_RU = {
    "Игра для Windows",
    "Закрыта из-за ошибки",
    "Перезапустить",
    "Выйти",
    "Код",
    "Модуль",
    "Нет файла",
    "В игре",
    "Настройки",
    "Вернуть прежние",
    "Оставить новые",
    "Рекомендованные",
    "Пробный запуск не удался",
    "Игра падает не первый раз подряд. Можно вернуть настройки по умолчанию.",
    {
        {
            "Игра перестала работать",
            "Игра столкнулась с ошибкой, с которой не смогла справиться, и закрылась. Всё, что игра сохранила "
            "до этого, на месте.",
            "Попробуйте запустить её снова. Если ошибка повторяется в одном и том же месте, загляните в настройки — "
            "там есть советы для этой ошибки.",
        },
        {
            "Игра попыталась сделать слишком много сразу",
            "Switch разрешает игре ограниченное число одновременных задач. Этой игре понадобилась ещё одна, "
            "и ей пришлось закрыться.",
            "Обычно помогает запустить игру ещё раз. Если ошибка повторяется каждый раз, загляните в настройки.",
        },
        {
            "Игре не хватило памяти",
            "Она запросила больше памяти, чем этой игре доступно на Switch, и закрылась.",
            "Уменьшите разрешение или качество текстур в настройках самой игры или загляните в настройки и "
            "запустите её снова.",
        },
        {
            "Не хватает файлов игры",
            "Игре нужен файл Windows, которого нет, поэтому она не может запуститься.",
            "Проверьте, что папка с игрой полная, а Windows DLL установлены в настройках Autorun.",
        },
        {
            "Среда Windows остановилась",
            "Часть среды выполнения Windows на Switch столкнулась с ошибкой, и игра закрылась.",
            "Запустите игру снова. Если ошибка повторяется каждый раз, проверьте настройки.",
        },
    },
    {
        "Игра упала с новыми настройками",
        "Это был пробный запуск: игра закрылась из-за ошибки после того, как её настройки поменяли. Прежние "
        "настройки сохранены.",
        "Верните прежние — игра запустится так, как работала до изменений. Или оставьте новые, если ошибка, похоже, "
        "не связана с ними.",
    },
};

static const Strings STRINGS_ZH_HANS = {
    "Windows 游戏",
    "因错误而关闭",
    "重新启动",
    "退出",
    "代码",
    "模块",
    "缺少文件",
    "游玩时间",
    "设置",
    "恢复旧的",
    "保留新的",
    "推荐设置",
    "试运行失败",
    "游戏已连续多次出错。可以恢复为默认设置。",
    {
        {
            "游戏停止运行了",
            "游戏遇到了无法恢复的问题，只能关闭。在此之前游戏保存的内容都会保留。",
            "请再次启动试试。如果总在同一个地方出错，请查看设置——里面有针对此错误的提示。",
        },
        {
            "游戏同时要做的事太多了",
            "Switch 只允许游戏同时运行有限数量的任务。这款游戏还需要再多一个，只能关闭。",
            "再次启动游戏通常就能解决。如果每次都出现，请检查系统设置。",
        },
        {
            "游戏内存不足",
            "游戏申请的内存超过了它在 Switch 上可用的数量，只能关闭。",
            "请在游戏选项中降低分辨率或纹理质量，或查看设置，然后再次启动。",
        },
        {
            "游戏缺少部分文件",
            "游戏缺少所需的 Windows 文件，因此无法启动。",
            "请确认游戏文件夹完整，且已在 Autorun 设置中安装 Windows DLL。",
        },
        {
            "Windows 环境已停止",
            "负责在 Switch 上运行 Windows 游戏的组件出错了，游戏只能关闭。",
            "请再次启动游戏。如果每次都出现，请检查设置。",
        },
    },
    {
        "游戏在新设置下崩溃了",
        "这是一次试运行：更改设置后，游戏因错误而关闭。之前运行时使用的设置仍然保留。",
        "恢复旧的设置，游戏就会像更改前一样启动；如果错误看起来与新设置无关，也可以保留新的设置。",
    },
};

static const Strings STRINGS_JA = {
    "Windowsのゲーム",
    "エラーで終了しました",
    "再起動",
    "終了",
    "コード",
    "モジュール",
    "不足ファイル",
    "プレイ時間",
    "設定",
    "元に戻す",
    "このまま",
    "おすすめ",
    "お試し起動に失敗",
    "ゲームが続けて何度も終了しています。既定の設定に戻せます。",
    {
        {
            "ゲームが動かなくなりました",
            "ゲームで回復できない問題が起きたため、終了しました。それまでにゲームが保存したデータは残っています。",
            "もう一度起動してみてください。同じ場所で何度も起きる場合は、設定を確認してください。"
            "このエラーへの対処法が載っています。",
        },
        {
            "ゲームが一度に多くの処理をしようとしました",
            "Switchでは、ゲームが同時に実行できる処理の数に限りがあります。"
            "このゲームはそれを1つ超えたため、終了しました。",
            "もう一度起動すると、たいてい解決します。毎回起きる場合は、設定を確認してください。",
        },
        {
            "メモリーが足りなくなりました",
            "このゲームがSwitchで使えるより多くのメモリーを必要としたため、終了しました。",
            "ゲームのオプションで解像度やテクスチャの品質を下げるか、設定を確認してから、もう一度起動してください。",
        },
        {
            "ゲームのファイルが足りません",
            "ゲームに必要なWindowsのファイルが見つからないため、起動できません。",
            "ゲームのフォルダーがそろっているかと、Windows DLLがインストールされているか確認してください。",
        },
        {
            "Windows環境が停止しました",
            "SwitchでWindowsのゲームを動かす環境でエラーが起き、ゲームが終了しました。",
            "もう一度ゲームを起動してください。毎回起きる場合は、設定を確認してください。",
        },
    },
    {
        "新しい設定でゲームがクラッシュしました",
        "これはお試し起動でした。設定を変更した後、ゲームがエラーで終了しました。"
        "以前の設定は保存されています。",
        "元に戻すと、変更前と同じ状態でゲームを起動します。"
        "エラーが新しい設定のせいではなさそうなら、このままにしてください。",
    },
};

static const Strings STRINGS_KO = {
    "Windows 게임",
    "오류로 종료됨",
    "다시 시작",
    "종료",
    "코드",
    "모듈",
    "없는 파일",
    "플레이 시간",
    "설정",
    "되돌리기",
    "새 설정 유지",
    "권장 설정",
    "시험 실행 실패",
    "게임이 여러 번 연달아 오류로 종료되었습니다. 기본 설정으로 되돌릴 수 있습니다.",
    {
        {
            "게임이 작동을 멈췄습니다",
            "게임에 복구할 수 없는 문제가 생겨 종료되었습니다. 그전에 게임이 저장한 내용은 그대로 "
            "남아 있습니다.",
            "다시 시작해 보세요. 같은 곳에서 계속 일어나면 설정을 확인하세요. 이 오류에 맞는 도움말이 "
            "있습니다.",
        },
        {
            "게임이 한 번에 너무 많은 일을 하려 했습니다",
            "Switch에서는 게임이 동시에 실행할 수 있는 작업 수가 정해져 있습니다. 이 게임은 작업이 하나 "
            "더 필요해서 종료되었습니다.",
            "보통 게임을 다시 시작하면 해결됩니다. 매번 일어난다면 설정을 확인해 보세요.",
        },
        {
            "게임의 메모리가 부족합니다",
            "이 게임이 Switch에서 쓸 수 있는 것보다 많은 메모리를 요청해서 종료되었습니다.",
            "게임 옵션에서 해상도나 텍스처 품질을 낮추거나 설정을 확인한 뒤 게임을 다시 시작하세요.",
        },
        {
            "게임 파일 일부가 없습니다",
            "게임에 필요한 Windows 파일이 없어 시작할 수 없습니다.",
            "게임 폴더에 빠진 파일이 없는지와 Windows DLL 설치 여부를 확인하세요.",
        },
        {
            "Windows 환경이 멈췄습니다",
            "Switch에서 Windows 게임을 실행하는 구성 요소에 오류가 생겨 게임이 종료되었습니다.",
            "게임을 다시 시작하세요. 매번 일어난다면 설정을 확인해 보세요.",
        },
    },
    {
        "새 설정으로 게임이 오류로 종료되었습니다",
        "시험 실행이었습니다. 설정을 바꾼 뒤 게임이 오류로 종료되었습니다. 전에 쓰던 설정은 그대로 "
        "보관되어 있습니다.",
        "되돌리면 바꾸기 전처럼 게임을 시작합니다. 오류가 새 설정 때문이 아닌 것 같다면 새 설정을 "
        "유지하세요.",
    },
};

static const Strings STRINGS_ZH_HANT = {
    "Windows 遊戲",
    "因錯誤而關閉",
    "重新啟動",
    "結束",
    "代碼",
    "模組",
    "缺少",
    "遊玩時間",
    "設定",
    "還原",
    "保留新的",
    "建議設定",
    "試執行失敗",
    "遊戲已連續多次失敗。可以恢復為預設設定。",
    {
        {
            "遊戲停止運作",
            "遊戲遇到無法復原的問題，只好關閉。遊戲在此之前儲存的內容都會保留。",
            "請再次啟動看看。如果總是在同一個地方發生，請查看設定，裡面有針對這個錯誤的建議。",
        },
        {
            "遊戲同時想做的事太多了",
            "Switch 只允許遊戲同時執行有限數量的工作。這款遊戲還需要多一個，只好關閉。",
            "再次啟動遊戲通常就能解決。如果每次都發生，請查看設定。",
        },
        {
            "遊戲的記憶體不足",
            "遊戲要求的記憶體超過它在 Switch 上能使用的量，只好關閉。",
            "請在遊戲的選項中降低解析度或材質品質，或查看設定，然後再次啟動。",
        },
        {
            "遊戲缺少部分檔案",
            "遊戲需要一個找不到的 Windows 檔案，因此無法啟動。",
            "請確認遊戲資料夾是否完整，以及是否已安裝 Windows DLL。",
        },
        {
            "Windows 環境已停止",
            "負責在 Switch 上執行 Windows 遊戲的元件發生錯誤，遊戲只好關閉。",
            "請再次啟動遊戲。如果每次都發生，請查看設定。",
        },
    },
    {
        "遊戲在新設定下當機了",
        "這是一次試執行：設定變更後，遊戲因錯誤而關閉。先前執行時使用的設定已保留。",
        "選擇還原，遊戲就會照變更前的方式啟動；如果錯誤似乎與新設定無關，也可以保留新的設定。",
    },
};

static const Strings STRINGS_FR = {
    "Jeu Windows",
    "Fermé après une erreur",
    "Redémarrer",
    "Quitter",
    "Code",
    "Module",
    "Manquant",
    "Temps de jeu",
    "Paramètres",
    "Rétablir",
    "Garder",
    "Recommandés",
    "Essai échoué",
    "Le jeu a planté plusieurs fois de suite. Vous pouvez revenir aux paramètres par défaut.",
    {
        {
            "Le jeu a cessé de fonctionner",
            "Le jeu a rencontré un problème dont il n'a pas pu se remettre et a dû se fermer. Tout ce que le jeu a "
            "sauvegardé avant est conservé.",
            "Essayez de le relancer. Si cela se reproduit au même endroit, regardez dans les paramètres : ils "
            "donnent des conseils pour cette erreur.",
        },
        {
            "Le jeu a tenté d'en faire trop à la fois",
            "La Switch ne laisse un jeu mener qu'un nombre limité de tâches en même temps. Ce jeu en a demandé une de "
            "plus et a dû se fermer.",
            "Relancer le jeu suffit en général. Si cela arrive à chaque fois, vérifiez les paramètres.",
        },
        {
            "Le jeu a manqué de mémoire",
            "Il a demandé plus de mémoire que ce jeu ne peut en utiliser sur la Switch et a dû se fermer.",
            "Baissez la résolution ou la qualité des textures dans les options du jeu, ou regardez dans les "
            "paramètres, puis relancez-le.",
        },
        {
            "Des fichiers du jeu sont manquants",
            "Le jeu a besoin d'un fichier Windows introuvable, il ne peut donc pas démarrer.",
            "Vérifiez que le dossier du jeu est complet et que les DLL Windows sont installées.",
        },
        {
            "L'environnement Windows s'est arrêté",
            "L'environnement qui exécute les jeux Windows sur la Switch a rencontré une erreur, et le jeu a "
            "dû se fermer.",
            "Relancez le jeu. Si cela arrive à chaque fois, vérifiez les paramètres.",
        },
    },
    {
        "Le jeu a planté avec les nouveaux paramètres",
        "C'était un lancement d'essai : le jeu s'est fermé à cause d'une erreur après la modification de ses "
        "paramètres. Les paramètres d'avant sont conservés.",
        "Rétablissez-les pour lancer le jeu comme avant la modification, ou gardez les nouveaux si l'erreur ne "
        "semble pas venir d'eux.",
    },
};

static const Strings STRINGS_FR_CA = {
    "Jeu Windows",
    "Fermé à cause d'une erreur",
    "Redémarrer",
    "Quitter",
    "Code",
    "Module",
    "Manquant",
    "Temps de jeu",
    "Paramètres",
    "Rétablir",
    "Garder",
    "Recommandés",
    "Essai échoué",
    "Le jeu a planté plusieurs fois de suite. Vous pouvez revenir aux paramètres par défaut.",
    {
        {
            "Le jeu a cessé de fonctionner",
            "Le jeu a rencontré un problème dont il n'a pas pu se remettre et a dû se fermer. Tout ce que le jeu a "
            "sauvegardé avant est conservé.",
            "Essayez de le redémarrer. Si cela se reproduit au même endroit, consultez les paramètres — ils "
            "contiennent des conseils pour cette erreur.",
        },
        {
            "Le jeu a tenté d'en faire trop à la fois",
            "La Switch ne permet à un jeu d'exécuter qu'un nombre limité de tâches en même temps. Ce jeu en a "
            "demandé une de plus et a dû se fermer.",
            "Redémarrer le jeu aide généralement. Si cela arrive chaque fois, consultez les paramètres.",
        },
        {
            "Le jeu a manqué de mémoire",
            "Il a demandé plus de mémoire que ce jeu ne peut en utiliser sur la Switch et a dû se fermer.",
            "Baissez la résolution ou la qualité des textures dans les options du jeu, ou consultez les paramètres, "
            "puis redémarrez-le.",
        },
        {
            "Des fichiers du jeu sont manquants",
            "Le jeu a besoin d'un fichier Windows introuvable, alors il ne peut pas démarrer.",
            "Vérifiez que le dossier du jeu est complet et que les DLL Windows sont installées.",
        },
        {
            "L'environnement Windows s'est arrêté",
            "L'environnement qui exécute les jeux Windows sur la Switch a rencontré une erreur, et le jeu a "
            "dû se fermer.",
            "Redémarrez le jeu. Si cela arrive chaque fois, vérifiez les paramètres.",
        },
    },
    {
        "Le jeu a planté avec les nouveaux paramètres",
        "C'était un lancement d'essai : le jeu s'est fermé à cause d'une erreur après la modification de ses "
        "paramètres. Les paramètres d'avant sont conservés.",
        "Rétablissez-les pour lancer le jeu comme avant la modification, ou gardez les nouveaux si l'erreur ne "
        "semble pas venir d'eux.",
    },
};

static const Strings STRINGS_DE = {
    "Windows-Spiel",
    "Mit Fehler beendet",
    "Neustart",
    "Beenden",
    "Code",
    "Modul",
    "Fehlt",
    "Gespielt",
    "Einstellungen",
    "Zurückholen",
    "Behalten",
    "Empfohlene",
    "Probelauf fehlgeschlagen",
    "Das Spiel ist mehrmals hintereinander abgestürzt. Du kannst auf die Standardeinstellungen zurückgehen.",
    {
        {
            "Das Spiel funktioniert nicht mehr",
            "Das Spiel hatte ein Problem, das es nicht beheben konnte, und musste beendet werden. Alles, was das Spiel "
            "vorher gespeichert hat, bleibt erhalten.",
            "Starte es noch einmal. Wenn es immer an derselben Stelle passiert, schau in die Einstellungen – dort gibt "
            "es Tipps zu diesem Fehler.",
        },
        {
            "Das Spiel wollte zu viel auf einmal",
            "Die Switch lässt ein Spiel nur eine begrenzte Zahl von Aufgaben gleichzeitig ausführen. Dieses Spiel "
            "brauchte eine mehr und musste beendet werden.",
            "Meist hilft es, das Spiel neu zu starten. Wenn es jedes Mal passiert, schau in die Einstellungen.",
        },
        {
            "Dem Spiel ist der Speicher ausgegangen",
            "Es hat mehr Speicher angefordert, als dieses Spiel auf der Switch nutzen kann, und musste beendet werden.",
            "Senke die Auflösung oder die Texturqualität in den Optionen des Spiels oder schau in die Einstellungen. "
            "Starte es dann noch einmal.",
        },
        {
            "Dateien des Spiels fehlen",
            "Das Spiel braucht eine Windows-Datei, die nicht gefunden wurde, und kann deshalb nicht starten.",
            "Prüfe, ob der Spielordner vollständig ist und die Windows-DLLs installiert sind.",
        },
        {
            "Die Windows-Umgebung wurde angehalten",
            "Die Windows-Laufzeit auf der Switch hatte einen Fehler, und das Spiel musste beendet werden.",
            "Starte das Spiel noch einmal. Wenn es jedes Mal passiert, prüfe die Einstellungen.",
        },
    },
    {
        "Das Spiel ist mit den neuen Einstellungen abgestürzt",
        "Das war ein Probelauf: Das Spiel wurde nach einer Änderung seiner Einstellungen mit einem Fehler beendet. Die "
        "Einstellungen, mit denen es vorher lief, sind aufbewahrt.",
        "Hol sie zurück, damit das Spiel so startet wie vor der Änderung, oder behalte die neuen, wenn der Fehler "
        "nicht an ihnen zu liegen scheint.",
    },
};

static const Strings STRINGS_IT = {
    "Gioco per Windows",
    "Chiuso per un errore",
    "Riavvia",
    "Esci",
    "Codice",
    "Modulo",
    "Mancante",
    "Giocato",
    "Impostazioni",
    "Ripristina",
    "Tieni nuove",
    "Consigliate",
    "Avvio di prova non riuscito",
    "Il gioco ha smesso di funzionare più volte di fila. Puoi tornare alle impostazioni predefinite.",
    {
        {
            "Il gioco ha smesso di funzionare",
            "Il gioco ha avuto un problema da cui non è riuscito a riprendersi e si è dovuto chiudere. Ciò che il "
            "gioco aveva salvato prima resta al sicuro.",
            "Prova ad avviarlo di nuovo. Se succede sempre nello stesso punto, guarda nelle impostazioni: ci sono "
            "consigli per questo errore.",
        },
        {
            "Il gioco ha provato a fare troppe cose insieme",
            "La Switch permette a un gioco di eseguire solo un numero limitato di attività contemporaneamente. Questo "
            "gioco ne ha chiesta una in più e si è dovuto chiudere.",
            "Di solito basta avviare di nuovo il gioco. Se succede ogni volta, guarda nelle impostazioni.",
        },
        {
            "Il gioco ha esaurito la memoria",
            "Ha chiesto più memoria di quanta questo gioco ne possa usare sulla Switch e si è dovuto chiudere.",
            "Abbassa la risoluzione o la qualità delle texture nelle opzioni del gioco, oppure guarda nelle "
            "impostazioni, poi avvialo di nuovo.",
        },
        {
            "Mancano alcuni file del gioco",
            "Il gioco ha bisogno di un file di Windows che non è stato trovato, quindi non può avviarsi.",
            "Controlla che la cartella del gioco sia completa e che le DLL di Windows siano installate.",
        },
        {
            "L'ambiente Windows si è fermato",
            "L'ambiente che esegue i giochi Windows sulla Switch ha avuto un errore e il gioco si è dovuto chiudere.",
            "Avvia di nuovo il gioco. Se succede ogni volta, guarda nelle impostazioni.",
        },
    },
    {
        "Il gioco è andato in crash con le nuove impostazioni",
        "Era un avvio di prova: il gioco si è chiuso per un errore dopo la modifica delle impostazioni. Le "
        "impostazioni con cui funzionava prima sono conservate.",
        "Ripristinale per avviare il gioco come funzionava prima della modifica, oppure tieni quelle nuove se l'errore "
        "non sembra dipendere da loro.",
    },
};

static const Strings STRINGS_ES = {
    "Juego de Windows",
    "Se ha cerrado por un error",
    "Reiniciar",
    "Salir",
    "Código",
    "Módulo",
    "Falta",
    "Jugado",
    "Ajustes",
    "Recuperar",
    "Mantener",
    "Recomendados",
    "Prueba fallida",
    "El juego ha fallado varias veces seguidas. Puedes volver a los ajustes predeterminados.",
    {
        {
            "El juego ha dejado de funcionar",
            "El juego ha tenido un problema del que no ha podido recuperarse y se ha cerrado. Se conserva todo lo "
            "que guardó antes.",
            "Prueba a iniciarlo de nuevo. Si vuelve a pasar en el mismo punto, consulta los ajustes: ahí hay "
            "consejos para este error.",
        },
        {
            "El juego ha intentado hacer demasiado a la vez",
            "La Switch solo deja que un juego realice un número limitado de tareas al mismo tiempo. Este juego "
            "necesitaba una más y ha tenido que cerrarse.",
            "Suele bastar con volver a iniciarlo. Si pasa siempre, revisa los ajustes.",
        },
        {
            "El juego se ha quedado sin memoria",
            "Ha pedido más memoria de la que este juego puede usar en la Switch y ha tenido que cerrarse.",
            "Baja la resolución o la calidad de las texturas en las opciones del juego, o consulta los ajustes, y "
            "vuelve a iniciarlo.",
        },
        {
            "Faltan archivos del juego",
            "El juego necesita un archivo de Windows que no se encuentra, así que no puede iniciarse.",
            "Comprueba que la carpeta del juego esté completa y que las DLL de Windows estén instaladas.",
        },
        {
            "El entorno de Windows se ha detenido",
            "El entorno que ejecuta juegos de Windows en la Switch ha tenido un error y el juego ha tenido "
            "que cerrarse.",
            "Vuelve a iniciar el juego. Si pasa siempre, revisa los ajustes.",
        },
    },
    {
        "El juego ha fallado con los ajustes nuevos",
        "Era un inicio de prueba: el juego se ha cerrado por un error después de cambiar sus ajustes. Se conservan "
        "los ajustes con los que funcionaba antes.",
        "Recupéralos para iniciar el juego como funcionaba antes del cambio, o mantén los nuevos si el error no "
        "parece deberse a ellos.",
    },
};

static const Strings STRINGS_ES_419 = {
    "Juego de Windows",
    "Se cerró por un error",
    "Reiniciar",
    "Salir",
    "Código",
    "Módulo",
    "Falta",
    "Tiempo de juego",
    "Configuración",
    "Restaurar",
    "Conservar",
    "Recomendada",
    "Falló el inicio de prueba",
    "El juego falló más de una vez seguida. Puedes volver a la configuración predeterminada.",
    {
        {
            "El juego dejó de funcionar",
            "El juego tuvo un problema del que no pudo recuperarse y tuvo que cerrarse. Todo lo que el juego guardó "
            "antes de esto se conserva.",
            "Intenta iniciarlo de nuevo. Si sigue pasando en el mismo punto, revisa la configuración: ahí hay consejos "
            "para este error.",
        },
        {
            "El juego intentó hacer demasiado a la vez",
            "La Switch permite que un juego ejecute solo una cantidad limitada de tareas al mismo tiempo. Este juego "
            "necesitó una más y tuvo que cerrarse.",
            "Volver a iniciar el juego suele ayudar. Si pasa cada vez, revisa la configuración.",
        },
        {
            "El juego se quedó sin memoria",
            "Pidió más memoria de la que este juego puede usar en la Switch y tuvo que cerrarse.",
            "Baja la resolución o la calidad de las texturas en las opciones del juego, o revisa la configuración, y "
            "vuelve a iniciarlo.",
        },
        {
            "Faltan archivos del juego",
            "El juego necesita un archivo de Windows que no se encuentra, así que no puede iniciar.",
            "Comprueba que la carpeta del juego esté completa y que las DLL de Windows estén instaladas.",
        },
        {
            "El entorno de Windows se detuvo",
            "El entorno que ejecuta juegos de Windows en la Switch tuvo un error y el juego tuvo que cerrarse.",
            "Vuelve a iniciar el juego. Si pasa cada vez, revisa la configuración.",
        },
    },
    {
        "El juego falló con la nueva configuración",
        "Era un inicio de prueba: el juego se cerró por un error después de cambiar su configuración. La "
        "configuración con la que funcionaba antes se conserva.",
        "Restáurala para iniciar el juego como funcionaba antes del cambio, o conserva la nueva si el error no parece "
        "deberse a ella.",
    },
};

static const Strings STRINGS_NL = {
    "Windows-game",
    "Afgesloten door een fout",
    "Herstarten",
    "Afsluiten",
    "Code",
    "Module",
    "Ontbreekt",
    "Gespeeld",
    "Instellingen",
    "Terugzetten",
    "Behouden",
    "Aanbevolen",
    "Proefstart mislukt",
    "De game is meer dan eens achter elkaar gecrasht. Je kunt teruggaan naar de standaardinstellingen.",
    {
        {
            "De game werkt niet meer",
            "De game liep tegen een probleem aan waarvan hij niet kon herstellen en moest afsluiten. Alles wat de game "
            "hiervoor heeft opgeslagen, blijft bewaard.",
            "Probeer hem opnieuw te starten. Gebeurt het steeds op dezelfde plek, kijk dan in de instellingen — daar "
            "staan tips voor deze fout.",
        },
        {
            "De game probeerde te veel tegelijk te doen",
            "De Switch laat een game maar een beperkt aantal taken tegelijk uitvoeren. Deze game had er nog één nodig "
            "en moest afsluiten.",
            "Opnieuw starten helpt meestal. Gebeurt het elke keer, bekijk dan de instellingen.",
        },
        {
            "De game had te weinig geheugen",
            "Hij vroeg meer geheugen dan deze game op de Switch kan gebruiken en moest afsluiten.",
            "Verlaag de resolutie of de textuurkwaliteit in de opties van de game, of kijk in de instellingen, en "
            "start hem dan opnieuw.",
        },
        {
            "Er ontbreken bestanden van de game",
            "De game heeft een Windows-bestand nodig dat ontbreekt, dus hij kan niet starten.",
            "Controleer of de gamemap compleet is en of de Windows-DLL's geïnstalleerd zijn.",
        },
        {
            "De Windows-omgeving is gestopt",
            "De omgeving die Windows-games op de Switch draait, liep tegen een fout aan en de game moest afsluiten.",
            "Start de game opnieuw. Gebeurt het elke keer, bekijk dan de instellingen.",
        },
    },
    {
        "De game is gecrasht met de nieuwe instellingen",
        "Dit was een proefstart: de game sloot af met een fout nadat de instellingen waren gewijzigd. De instellingen "
        "waarmee hij eerder draaide, zijn bewaard.",
        "Zet ze terug om de game te starten zoals hij werkte vóór de wijziging, of behoud de nieuwe als de fout er "
        "niet door lijkt te komen.",
    },
};

static const Strings STRINGS_PT = {
    "Jogo para Windows",
    "Fechado devido a um erro",
    "Reiniciar",
    "Sair",
    "Código",
    "Módulo",
    "Em falta",
    "Tempo de jogo",
    "Definições",
    "Repor",
    "Manter novas",
    "Recomendadas",
    "Arranque de teste falhou",
    "O jogo falhou mais do que uma vez seguida. Pode voltar às definições predefinidas.",
    {
        {
            "O jogo deixou de funcionar",
            "O jogo encontrou um problema do qual não conseguiu recuperar e teve de fechar. Tudo o que o jogo "
            "guardou antes disto foi mantido.",
            "Tente iniciá-lo de novo. Se voltar a acontecer no mesmo ponto, consulte as definições — têm sugestões "
            "para este erro.",
        },
        {
            "O jogo tentou fazer demasiadas coisas ao mesmo tempo",
            "A Switch só deixa um jogo executar um número limitado de tarefas em simultâneo. Este jogo precisou de "
            "mais uma e teve de fechar.",
            "Iniciar o jogo de novo costuma ajudar. Se acontecer sempre, consulte as definições.",
        },
        {
            "O jogo ficou sem memória",
            "Pediu mais memória do que este jogo pode usar na Switch e teve de fechar.",
            "Baixe a resolução ou a qualidade das texturas nas opções do jogo, ou consulte as definições, e depois "
            "inicie-o de novo.",
        },
        {
            "Faltam ficheiros do jogo",
            "O jogo precisa de um ficheiro do Windows em falta, por isso não consegue arrancar.",
            "Confirme que a pasta do jogo está completa e que as DLLs do Windows estão instaladas.",
        },
        {
            "O ambiente Windows parou",
            "O ambiente que executa jogos para Windows na Switch encontrou um erro, e o jogo teve de fechar.",
            "Inicie o jogo de novo. Se acontecer sempre, consulte as definições.",
        },
    },
    {
        "O jogo falhou com as novas definições",
        "Este foi um arranque de teste: o jogo fechou devido a um erro depois de as definições terem sido "
        "alteradas. As definições com que funcionava antes foram mantidas.",
        "Reponha-as para iniciar o jogo como funcionava antes da alteração, ou mantenha as novas se o erro não "
        "parecer ser causado por elas.",
    },
};

static const Strings STRINGS_PT_BR = {
    "Jogo para Windows",
    "Fechou com um erro",
    "Reiniciar",
    "Sair",
    "Código",
    "Módulo",
    "Ausente",
    "Tempo de jogo",
    "Configurar",
    "Restaurar",
    "Manter novas",
    "Recomendadas",
    "Teste falhou",
    "O jogo falhou mais de uma vez seguida. Você pode voltar às configurações padrão.",
    {
        {
            "O jogo parou de funcionar",
            "O jogo encontrou um problema do qual não conseguiu se recuperar e precisou fechar. Tudo o que o jogo "
            "salvou antes disso foi mantido.",
            "Tente iniciá-lo de novo. Se continuar acontecendo no mesmo ponto, veja as configurações — lá há dicas "
            "para este erro.",
        },
        {
            "O jogo tentou fazer coisas demais ao mesmo tempo",
            "O Switch só deixa um jogo executar um número limitado de tarefas ao mesmo tempo. Este jogo precisou de "
            "mais uma e teve que fechar.",
            "Iniciar o jogo de novo costuma resolver. Se acontecer toda vez, veja as configurações.",
        },
        {
            "O jogo ficou sem memória",
            "Ele pediu mais memória do que este jogo pode usar no Switch e teve que fechar.",
            "Diminua a resolução ou a qualidade das texturas nas opções do jogo, ou veja as configurações, e depois "
            "inicie-o de novo.",
        },
        {
            "Faltam arquivos do jogo",
            "O jogo precisa de um arquivo do Windows que está ausente, por isso não pode iniciar.",
            "Verifique se a pasta do jogo está completa e se as DLLs do Windows estão instaladas.",
        },
        {
            "O ambiente Windows parou",
            "O ambiente que roda jogos de Windows no Switch encontrou um erro, e o jogo teve que fechar.",
            "Inicie o jogo de novo. Se acontecer toda vez, veja as configurações.",
        },
    },
    {
        "O jogo falhou com as novas configurações",
        "Este foi um teste: o jogo fechou com um erro depois que as configurações foram alteradas. As "
        "configurações com que ele rodava antes foram mantidas.",
        "Restaure-as para iniciar o jogo como funcionava antes da mudança, ou mantenha as novas se o erro não "
        "parecer ser causado por elas.",
    },
};

static const Strings* const STRINGS[CarafeLanguageCount] = {
    [CarafeLanguageRu] = &STRINGS_RU,
    [CarafeLanguageEn] = &STRINGS_EN,
    [CarafeLanguageZhHans] = &STRINGS_ZH_HANS,
    [CarafeLanguageJa] = &STRINGS_JA,
    [CarafeLanguageKo] = &STRINGS_KO,
    [CarafeLanguageZhHant] = &STRINGS_ZH_HANT,
    [CarafeLanguageFr] = &STRINGS_FR,
    [CarafeLanguageFrCa] = &STRINGS_FR_CA,
    [CarafeLanguageDe] = &STRINGS_DE,
    [CarafeLanguageIt] = &STRINGS_IT,
    [CarafeLanguageEs] = &STRINGS_ES,
    [CarafeLanguageEs419] = &STRINGS_ES_419,
    [CarafeLanguageNl] = &STRINGS_NL,
    [CarafeLanguagePt] = &STRINGS_PT,
    [CarafeLanguagePtBr] = &STRINGS_PT_BR,
};

static const struct {
    unsigned int code;
    const char* name;
} CODE_NAMES[] = {
    { 0x00000003u, "abort" },
    { 0x80000003u, "breakpoint" },
    { 0xC0000005u, "access violation" },
    { 0xC0000017u, "no memory" },
    { 0xC000001Du, "illegal instruction" },
    { 0xC000007Bu, "bad image" },
    { 0xC0000094u, "integer divide by zero" },
    { 0xC00000FDu, "stack overflow" },
    { 0xC000012Du, "commitment limit" },
    { 0xC0000135u, "DLL not found" },
    { 0xC0000139u, "entry point not found" },
    { 0xC0000142u, "DLL init failed" },
    { 0xC0000409u, "stack buffer overrun" },
    { 0xE06D7363u, "C++ exception" },
};

extern char __end__[];
extern u32 __nx_applet_exit_mode;
extern int wine_nx_image_at(const void* addr, void** base, char* name, size_t size) __attribute__((weak));
extern const char* wine_nx_loader_last_import_dll(void) __attribute__((weak));
extern void wine_nx_runtime_open_error_log(void) __attribute__((weak));
int __real_main(int argc, char** argv);

static u64 g_start_tick;
static unsigned int g_exception_code;
static char g_exception_module[NAME_SIZE];
static bool g_exception_seen;
static char g_missing[NAME_SIZE];
static unsigned int g_threads_refused;
static unsigned int g_native_esr;
static uintptr_t g_native_pc;
static bool g_native_seen;
static bool g_report_written;
static bool g_error_mode;

static const Strings* g_strings = &STRINGS_EN;
static CarafeImage g_icon;
static char g_title[NAME_SIZE];

static void trace(const char* line) {
    carafeOverlayLog(line);
}

static void gameExecutable(char* out, size_t size) {
    char target[PATH_SIZE] = "";
    snprintf(out, size, "exe");
    if (carafeOverlayProgramTarget == NULL || !carafeOverlayProgramTarget(target, sizeof(target))) {
        char argv[ARGV_SIZE];
        if (!carafeOverlayReadRomfsText("/carafe/argv", argv, sizeof(argv))) {
            return;
        }
        snprintf(target, sizeof(target), "%s", argv);
    }
    char* end = NULL;
    for (char* at = target; (at = strchr(at, '.')) != NULL; at++) {
        if (strncasecmp(at, ".exe", 4) == 0) {
            end = at + 4;
        }
    }
    if (end == NULL) {
        return;
    }
    char* start = end - 4;
    while (start > target && start[-1] != '/' && start[-1] != '\\' && start[-1] != '"') {
        start--;
    }
    snprintf(out, size, "%.*s", (int)(end - start), start);
}

void carafeErrorResetStartTick(void) {
    g_start_tick = armGetSystemTick();
    remove(PENDING_ERROR_FILE);
}

void carafeErrorException(unsigned int code, const void* address) {
    g_exception_code = code;
    g_exception_module[0] = '\0';
    if (wine_nx_image_at != NULL) {
        void* base = NULL;
        char name[NAME_SIZE];
        if (wine_nx_image_at(address, &base, name, sizeof(name)) == 0) {
            if (name[0] == '\0') {
                gameExecutable(name, sizeof(name));
            }
            snprintf(g_exception_module, sizeof(g_exception_module), "%s + 0x%lx", name,
                (unsigned long)((uintptr_t)address - (uintptr_t)base));
        }
    }
    g_exception_seen = true;
}

void carafeErrorMissingDll(const char* name) {
    if (name != NULL) {
        snprintf(g_missing, sizeof(g_missing), "%s", name);
    }
}

void carafeErrorThreadRefused(void) {
    __atomic_add_fetch(&g_threads_refused, 1, __ATOMIC_RELAXED);
}

bool carafeErrorNative(unsigned int esr, unsigned long pc) {
    if (g_error_mode || __atomic_exchange_n(&g_native_seen, true, __ATOMIC_SEQ_CST)) {
        return false;
    }
    g_native_esr = esr;
    g_native_pc = pc;
    return true;
}

static bool exitFailed(unsigned int code) {
    return code == 3u || code >= 0x80000000u;
}

static Kind classify(unsigned int code) {
    if (g_native_seen) {
        return KindSystem;
    }
    switch (code) {
    case 0xC0000017u:
    case 0xC000012Du:
        return KindMemory;
    case 0xC000007Bu:
    case 0xC0000135u:
    case 0xC0000139u:
    case 0xC0000142u:
        return KindSetup;
    default:
        break;
    }
    return __atomic_load_n(&g_threads_refused, __ATOMIC_RELAXED) ? KindThreads : KindCrash;
}

static void describeRuntimeAddress(uintptr_t address, char* out, size_t size) {
    MemoryInfo info;
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, (u64)(uintptr_t)&describeRuntimeAddress))
        && address >= info.addr && address < (uintptr_t)__end__) {
        snprintf(out, size, "wine-nx-runtime + 0x%lx", (unsigned long)(address - info.addr));
    } else {
        snprintf(out, size, "0x%lx", (unsigned long)address);
    }
}

static void describeModule(Kind kind, unsigned int code, char* out, size_t size) {
    out[0] = '\0';
    if (kind == KindSystem) {
        describeRuntimeAddress(g_native_pc, out, size);
        return;
    }
    if (g_exception_seen && g_exception_code == code) {
        snprintf(out, size, "%s", g_exception_module);
    }
}

static void reportPath(char* out, size_t size) {
    char dir[PATH_SIZE];
    if (carafeOverlayProgramDir != NULL && carafeOverlayProgramDir(dir, sizeof(dir))) {
        snprintf(out, size, "%s/carafe-error.txt", dir);
        return;
    }
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/wine", 0777);
    mkdir("sdmc:/switch/wine/carafe", 0777);
    snprintf(out, size, "sdmc:/switch/wine/carafe/carafe-error.txt");
}

static void writePendingMeta(const char* own_nro, bool restart_pending) {
    char target[512] = "";
    char title[128] = "";
    char icon[512] = "";
    char meta_nro[512] = "";
    bool forwarded = false;
    bool reopen_launcher = true;

    if (carafeOverlayGetMeta != NULL) {
        carafeOverlayGetMeta(target, sizeof(target), title, sizeof(title), icon, sizeof(icon),
            meta_nro, sizeof(meta_nro), &forwarded, &reopen_launcher);
    }
    const char* nro = (own_nro != NULL && own_nro[0] != '\0') ? own_nro : meta_nro;

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/wine", 0777);
    mkdir("sdmc:/switch/wine/carafe", 0777);
    FILE* file = fopen(PENDING_ERROR_FILE, "wb");
    if (file == NULL) {
        return;
    }
    fprintf(file, "target=%s\ntitle=%s\nicon=%s\nown_nro=%s\nforwarded=%d\nreopen_launcher=%d\nrestart_pending=%d\n",
        target, title, icon, nro, forwarded ? 1 : 0, reopen_launcher ? 1 : 0, restart_pending ? 1 : 0);
    fclose(file);
}

static bool readPendingMeta(bool* restart_pending) {
    FILE* file = fopen(PENDING_ERROR_FILE, "rb");
    if (file == NULL) {
        return false;
    }
    char target[512] = "";
    char title[128] = "";
    char icon[512] = "";
    char own_nro[512] = "";
    bool forwarded = false;
    bool reopen_launcher = true;
    bool pending = false;

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char* val = strchr(line, '=');
        if (val == NULL) {
            continue;
        }
        *val++ = '\0';
        if (strcmp(line, "target") == 0) {
            snprintf(target, sizeof(target), "%s", val);
        } else if (strcmp(line, "title") == 0) {
            snprintf(title, sizeof(title), "%s", val);
        } else if (strcmp(line, "icon") == 0) {
            snprintf(icon, sizeof(icon), "%s", val);
        } else if (strcmp(line, "own_nro") == 0) {
            snprintf(own_nro, sizeof(own_nro), "%s", val);
        } else if (strcmp(line, "forwarded") == 0) {
            forwarded = atoi(val) != 0;
        } else if (strcmp(line, "reopen_launcher") == 0) {
            reopen_launcher = atoi(val) != 0;
        } else if (strcmp(line, "restart_pending") == 0) {
            pending = atoi(val) != 0;
        }
    }
    fclose(file);

    if (restart_pending != NULL) {
        *restart_pending = pending;
    }
    if (carafeOverlaySetMeta != NULL && target[0] != '\0') {
        carafeOverlaySetMeta(target, title, icon, own_nro, forwarded, reopen_launcher);
    }
    return target[0] != '\0';
}

static bool writeReport(unsigned int exit_code) {
    if (g_report_written) {
        return true;
    }
    if (!g_native_seen && (exit_code == ~0u || !exitFailed(exit_code))) {
        return false;
    }

    Kind kind = classify(exit_code);
    unsigned int code = kind == KindSystem ? g_native_esr : exit_code;
    char module[NAME_SIZE];
    describeModule(kind, exit_code, module, sizeof(module));
    u64 now = armGetSystemTick();
    unsigned int seconds = g_start_tick != 0 ? (unsigned int)(armTicksToNs(now - g_start_tick) / 1000000000ULL) : 0;

    if (kind == KindSetup && g_missing[0] == '\0' && wine_nx_loader_last_import_dll != NULL) {
        const char* imp = wine_nx_loader_last_import_dll();
        if (imp != NULL && imp[0] != '\0') {
            snprintf(g_missing, sizeof(g_missing), "%s", imp);
        }
    }

    char path[PATH_SIZE];
    reportPath(path, sizeof(path));
    FILE* file = fopen(path, "w");
    if (file == NULL) {
        trace("[CARAFE] the game failed, but its error could not be written down");
        return false;
    }
    fprintf(file, "kind=%d\ncode=%08x\nseconds=%u\n", (int)kind, code, seconds);
    if (module[0] != '\0') {
        fprintf(file, "module=%s\n", module);
    }
    if (kind == KindSetup && g_missing[0] != '\0') {
        fprintf(file, "missing=%s\n", g_missing);
    }
    if (carafeTrialActive()) {
        fprintf(file, "trial=1\n");
    }
    fclose(file);
    g_report_written = true;
    unsigned int failures = carafeFailuresAdd();

    char line[LINE_SIZE];
    snprintf(line, sizeof(line), "[CARAFE] the game failed (kind %d, code 0x%08x%s%s), %u in a row", (int)kind, code,
        module[0] != '\0' ? ", " : "", module, failures);
    trace(line);
    return true;
}

static bool pushRunNext(const char* target) {
    if (target == NULL || target[0] == '\0') {
        return false;
    }
    char rest[2048];
    size_t got = 0;
    FILE* file = fopen(RUN_NEXT_FILE, "r");
    if (file != NULL) {
        got = fread(rest, 1, sizeof(rest) - 1, file);
        fclose(file);
    }
    rest[got] = '\0';
    size_t tlen = strlen(target);
    if (strncmp(rest, target, tlen) == 0 && (rest[tlen] == '\0' || rest[tlen] == '\n' || rest[tlen] == '\r')) {
        return true;
    }
    file = fopen(RUN_NEXT_FILE, "w");
    if (file == NULL) {
        return false;
    }
    bool ok = fprintf(file, "%s\n%s", target, rest) > 0;
    return fclose(file) == 0 && ok;
}

static void popRunNextIfMatches(const char* target) {
    if (target == NULL || target[0] == '\0') {
        return;
    }
    char text[2048];
    size_t got = 0;
    FILE* file = fopen(RUN_NEXT_FILE, "r");
    if (file == NULL) {
        return;
    }
    got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = '\0';
    size_t tlen = strlen(target);
    if (strncmp(text, target, tlen) != 0 || (text[tlen] != '\0' && text[tlen] != '\n' && text[tlen] != '\r')) {
        return;
    }
    char* rest = text + tlen;
    while (*rest == '\r' || *rest == '\n') {
        rest++;
    }
    if (*rest == '\0') {
        remove(RUN_NEXT_FILE);
    } else if ((file = fopen(RUN_NEXT_FILE, "w")) != NULL) {
        fputs(rest, file);
        fclose(file);
    }
}

bool carafeErrorRelaunch(const char* own_nro, unsigned int exit_code) {
    if (carafeRestartRequested()) {
        return false;
    }
    if (!writeReport(exit_code)) {
        carafeFailuresClear();
        return false;
    }
    if (own_nro == NULL || own_nro[0] == '\0' || !envHasNextLoad()) {
        return false;
    }
    writePendingMeta(own_nro, false);
    carafeOverlayCommit("before showing the error screen");
    char argv[ARGV_SIZE];
    snprintf(argv, sizeof(argv), "\"%s\" %s", own_nro, ERROR_FLAG);
    if (R_FAILED(envSetNextLoad(own_nro, argv))) {
        remove(PENDING_ERROR_FILE);
        trace("[CARAFE] the error screen cannot be started from here; restarting instead");
        return false;
    }
    trace("[CARAFE] starting this program again for the error screen");
    return true;
}

bool carafeErrorRestart(unsigned int exit_code) {
    if (!carafeRestartRequested() && !writeReport(exit_code)) {
        carafeFailuresClear();
        return false;
    }
    bool pushed = false;
    char target[512] = "";
    if (carafeRestartRequested()) {
        if (carafeOverlayProgramTarget != NULL && carafeOverlayProgramTarget(target, sizeof(target))
            && target[0] != '\0') {
            pushed = pushRunNext(target);
        }
    }
    writePendingMeta(NULL, !carafeRestartRequested());
    carafeOverlayCommit("before restarting the application for the error screen");
    trace(carafeRestartRequested()
            ? "[CARAFE] the loader cannot take the process back; restarting the application with the new settings"
            : "[CARAFE] the loader cannot take the process back; restarting the application for the error screen");
    Result rc = appletRestartProgram(NULL, 0);
    if (R_FAILED(rc)) {
        remove(PENDING_ERROR_FILE);
        if (pushed) {
            popRunNextIfMatches(target);
        }
        char line[LINE_SIZE];
        snprintf(line, sizeof(line), "[CARAFE] the application cannot restart (rc=0x%08x)", (unsigned int)rc);
        trace(line);
    }
    return R_SUCCEEDED(rc);
}

static bool readReport(Report* report) {
    char path[PATH_SIZE];
    reportPath(path, sizeof(path));
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    memset(report, 0, sizeof(*report));
    char line[LINE_SIZE];
    bool kind_seen = false;
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char* value = strchr(line, '=');
        if (value == NULL) {
            continue;
        }
        *value++ = '\0';
        if (strcmp(line, "kind") == 0) {
            int kind = atoi(value);
            if (kind >= 0 && kind < KindCount) {
                report->kind = (Kind)kind;
                kind_seen = true;
            }
        } else if (strcmp(line, "code") == 0) {
            report->code = (unsigned int)strtoul(value, NULL, 16);
        } else if (strcmp(line, "seconds") == 0) {
            report->seconds = (unsigned int)strtoul(value, NULL, 10);
        } else if (strcmp(line, "module") == 0) {
            snprintf(report->module, sizeof(report->module), "%s", value);
        } else if (strcmp(line, "missing") == 0) {
            snprintf(report->missing, sizeof(report->missing), "%s", value);
        } else if (strcmp(line, "trial") == 0) {
            report->trial = atoi(value) != 0;
        }
    }
    fclose(file);
    return kind_seen;
}

static void loadTitle(void) {
    snprintf(g_title, sizeof(g_title), "%s", g_strings->fallback_title);
    char title[NAME_SIZE];
    if (carafeOverlayReadRomfsText("/carafe/title", title, sizeof(title))) {
        title[strcspn(title, "\r\n")] = '\0';
        if (title[0] != '\0') {
            snprintf(g_title, sizeof(g_title), "%s", title);
        }
    }
}

static void drawIcon(CarafeImage* picture, int left, int top) {
    if (g_icon.pixels == NULL) {
        carafeDrawRound(picture, left, top, ICON_SIZE, ICON_SIZE, ICON_RADIUS, CARAFE_COLOR_ACCENT, 255);
        char initial[8] = "";
        const unsigned char* first = (const unsigned char*)g_title;
        size_t length = *first >= 0xF0 ? 4 : *first >= 0xE0 ? 3 : *first >= 0xC0 ? 2 : 1;
        snprintf(initial, sizeof(initial), "%.*s", (int)length, g_title);
        CarafeTextStyle style = { 64, CarafeHeavy, ICON_SIZE, CARAFE_COLOR_INK, true };
        carafeDrawLine(picture, initial, &style, left, top, ICON_SIZE);
        return;
    }
    carafeDrawPicture(picture, &g_icon, left, top, ICON_SIZE, ICON_RADIUS);
}

static void codeText(const Report* report, char* out, size_t size) {
    if (report->kind == KindSystem) {
        snprintf(out, size, "ESR 0x%08X", report->code);
        return;
    }
    for (size_t i = 0; i < sizeof(CODE_NAMES) / sizeof(CODE_NAMES[0]); i++) {
        if (CODE_NAMES[i].code == report->code) {
            snprintf(out, size, report->code < 0x100 ? "%u %s" : "0x%08X %s", report->code, CODE_NAMES[i].name);
            return;
        }
    }
    snprintf(out, size, report->code < 0x100 ? "%u" : "0x%08X", report->code);
}

static bool repeated(const Report* report) {
    return !report->trial && report->kind != KindSetup && carafeFailures() >= 2;
}

static bool settingsAvailable(const Report* report) {
    size_t menu_size = 0;
    if (report->kind == KindSetup) {
        return false;
    }
    void* menu_data = carafeOverlayReadRomfsFile("/carafe/menu.txt", 1024 * 1024, &menu_size);
    bool ok = menu_data != NULL && menu_size > 0;
    free(menu_data);
    return ok;
}

static int layoutButtons(const Report* report, Button* buttons) {
    int count = 0;
    if (report->trial) {
        buttons[count++] = (Button){ ChoiceRevert, 0, 0, 0, g_strings->revert };
        buttons[count++] = (Button){ ChoiceKeep, 0, 0, 0, g_strings->keep };
    } else if (repeated(report)) {
        buttons[count++] = (Button){ ChoiceRecommended, 0, 0, 0, g_strings->recommended };
        if (settingsAvailable(report)) {
            buttons[count++] = (Button){ ChoiceSettings, 0, 0, 0, g_strings->settings };
        }
    } else {
        if (report->kind != KindSetup) {
            buttons[count++] = (Button){ ChoiceRestart, 0, 0, 0, g_strings->restart };
        }
        if (settingsAvailable(report)) {
            buttons[count++] = (Button){ ChoiceSettings, 0, 0, 0, g_strings->settings };
        }
    }
    buttons[count++] = (Button){ ChoiceExit, 0, 0, 0, g_strings->exit };
    int x = MARGIN;
    for (int i = 0; i < count; i++) {
        buttons[i].x = x;
        buttons[i].y = SCREEN_HEIGHT - MARGIN - BUTTON_HEIGHT;
        buttons[i].width = BUTTON_FOCUS_PAD + GLYPH_SIZE + 12 + carafeTextWidth(buttons[i].label, 22, CarafeBold)
            + BUTTON_PAD;
        x += buttons[i].width + BUTTON_GAP;
    }
    return count;
}

static void drawButton(CarafeImage* picture, const Button* button, bool focused) {
    CarafeTextStyle label = { 22, CarafeBold, BUTTON_HEIGHT, focused ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_INK,
        false };
    if (!focused) {
        carafeDrawRound(picture, button->x, button->y, button->width, BUTTON_HEIGHT, BUTTON_RADIUS, CARAFE_COLOR_WHITE,
            18);
        carafeDrawRoundStroke(picture, button->x, button->y, button->width, BUTTON_HEIGHT, BUTTON_RADIUS, 2,
            CARAFE_COLOR_WHITE, 36);
        label.centered = true;
        carafeDrawLine(picture, button->label, &label, button->x, button->y, button->width);
        return;
    }
    carafeDrawFocusRing(picture, button->x, button->y, button->width, BUTTON_HEIGHT, BUTTON_RADIUS);
    int glyph_x = button->x + BUTTON_FOCUS_PAD;
    int glyph_y = button->y + (BUTTON_HEIGHT - GLYPH_SIZE) / 2;
    carafeDrawRound(picture, glyph_x, glyph_y, GLYPH_SIZE, GLYPH_SIZE, GLYPH_SIZE / 2, CARAFE_COLOR_PRIMARY_INK, 255);
    CarafeTextStyle glyph = { 19, CarafeHeavy, GLYPH_SIZE, CARAFE_COLOR_INK, true };
    carafeDrawLine(picture, "A", &glyph, glyph_x, glyph_y, GLYPH_SIZE);
    carafeDrawLine(picture, button->label, &label, glyph_x + GLYPH_SIZE + 12, button->y, 0);
}

static void drawCard(CarafeImage* picture, const Report* report) {
    const char* labels[MAX_ROWS];
    char values[MAX_ROWS][NAME_SIZE];
    int rows = 0;
    if (report->missing[0] != '\0') {
        labels[rows] = g_strings->missing;
        snprintf(values[rows++], NAME_SIZE, "%s", report->missing);
    }
    labels[rows] = g_strings->code;
    codeText(report, values[rows++], NAME_SIZE);
    if (report->module[0] != '\0') {
        labels[rows] = g_strings->module;
        snprintf(values[rows++], NAME_SIZE, "%s", report->module);
    }
    labels[rows] = g_strings->played;
    snprintf(values[rows++], NAME_SIZE, "%02u:%02u", report->seconds / 60, report->seconds % 60);

    const CarafeTextStyle label_style = { 17, CarafeRegular, 26, CARAFE_COLOR_LABEL, false };
    const CarafeTextStyle value_style = { 17, CarafeRegular, 26, CARAFE_COLOR_VALUE, false };
    const int inner_x = CARD_X + CARD_PAD_X;
    const int inner_width = CARD_WIDTH - 2 * CARD_PAD_X;
    int label_width = 0;
    for (int i = 0; i < rows; i++) {
        int w = carafeTextWidth(labels[i], label_style.size, label_style.weight);
        label_width = w > label_width ? w : label_width;
    }
    const int value_x = inner_x + label_width + 18;
    const int value_width = inner_width - label_width - 18;
    int details_height = 0;
    for (int i = 0; i < rows; i++) {
        details_height += carafeWrappedHeight(values[i], &value_style, value_width) + (i ? 6 : 0);
    }

    const int card_y = MARGIN;
    const int card_height = SCREEN_HEIGHT - 2 * MARGIN;
    carafeDrawRound(picture, CARD_X, card_y, CARD_WIDTH, card_height, CARD_RADIUS, CARAFE_COLOR_PANEL, 107);
    carafeDrawRoundStroke(picture, CARD_X, card_y, CARD_WIDTH, card_height, CARD_RADIUS, 1, CARAFE_COLOR_WHITE, 20);

    int y = card_y + CARD_PAD_Y;
    drawIcon(picture, CARD_X + (CARD_WIDTH - ICON_SIZE) / 2, y);
    y += ICON_SIZE + 22;
    const CarafeTextStyle name = { 26, CarafeBold, 31, CARAFE_COLOR_INK, true };
    y = carafeDrawWrapped(picture, g_title, &name, inner_x, y, inner_width) + 14;
    const char* chip_text = report->trial ? g_strings->trial_chip : g_strings->closed;
    const CarafeTextStyle chip = { 16, CarafeBold, 34, CARAFE_COLOR_CHIP, true };
    int chip_width = carafeTextWidth(chip_text, chip.size, chip.weight) + 28;
    int chip_x = CARD_X + (CARD_WIDTH - chip_width) / 2;
    carafeDrawRound(picture, chip_x, y, chip_width, chip.line_height, chip.line_height / 2, CARAFE_COLOR_FOCUS, 41);
    carafeDrawLine(picture, chip_text, &chip, chip_x, y, chip_width);

    y = card_y + card_height - CARD_PAD_Y - details_height;
    carafeDrawRound(picture, inner_x, y - 18, inner_width, 1, 0, CARAFE_COLOR_WHITE, 40);
    for (int i = 0; i < rows; i++) {
        carafeDrawLine(picture, labels[i], &label_style, inner_x, y, 0);
        y = carafeDrawWrapped(picture, values[i], &value_style, value_x, y, value_width) + 6;
    }
}

static void render(CarafeImage* picture, const Report* report, const Button* buttons, int count, int focus) {
    const KindText* text = report->trial ? &g_strings->trial : &g_strings->kinds[report->kind];
    const CarafeTextStyle title = { 40, CarafeHeavy, 46, CARAFE_COLOR_INK, false };
    const CarafeTextStyle body = { 22, CarafeRegular, 32, CARAFE_COLOR_TEXT, false };
    const CarafeTextStyle hint = { 19, CarafeRegular, 28, CARAFE_COLOR_HINT, false };
    carafeDrawBackground(picture, 255);

    int y = MARGIN;
    y = carafeDrawWrapped(picture, text->title, &title, MARGIN, y, TEXT_WIDTH) + 18;
    y = carafeDrawWrapped(picture, text->body, &body, MARGIN, y, TEXT_WIDTH) + 18;
    carafeDrawWrapped(picture, repeated(report) ? g_strings->again : text->hint, &hint, MARGIN, y, TEXT_WIDTH);

    for (int i = 0; i < count; i++) {
        drawButton(picture, &buttons[i], i == focus);
    }
    drawCard(picture, report);
}

static void present(Framebuffer* framebuffer, const CarafeImage* picture) {
    u32 stride = 0;
    uint32_t* out = framebufferBegin(framebuffer, &stride);
    if (out == NULL) {
        return;
    }
    for (int y = 0; y < picture->height; y++) {
        const uint32_t* row = picture->pixels + y * picture->width;
        uint32_t* target = (uint32_t*)((uint8_t*)out + (size_t)y * stride);
        for (int x = 0; x < picture->width; x++) {
            uint32_t argb = row[x];
            target[x] = 0xFF000000u | (argb & 0x0000FF00u) | (argb >> 16 & 0xFFu) | (argb & 0xFFu) << 16;
        }
    }
    framebufferEnd(framebuffer);
}

static int buttonAt(const Button* buttons, int count, int x, int y) {
    for (int i = 0; i < count; i++) {
        if (x >= buttons[i].x && x < buttons[i].x + buttons[i].width && y >= buttons[i].y
            && y < buttons[i].y + BUTTON_HEIGHT) {
            return i;
        }
    }
    return -1;
}

static unsigned int oskButtons(u64 held) {
    static const struct {
        u64 button;
        unsigned int osk;
    } MAP[] = {
        { HidNpadButton_Up, OSK_UP }, { HidNpadButton_Down, OSK_DOWN }, { HidNpadButton_Left, OSK_LEFT },
        { HidNpadButton_Right, OSK_RIGHT }, { HidNpadButton_A, OSK_A }, { HidNpadButton_B, OSK_B },
        { HidNpadButton_X, OSK_X }, { HidNpadButton_Y, OSK_Y }, { HidNpadButton_L, OSK_L },
        { HidNpadButton_R, OSK_R }, { HidNpadButton_ZL, OSK_ZL }, { HidNpadButton_ZR, OSK_ZR },
        { HidNpadButton_Plus, OSK_PLUS }, { HidNpadButton_Minus, OSK_MINUS },
        { HidNpadButton_StickL, OSK_STICKL }, { HidNpadButton_StickR, OSK_STICKR },
    };
    unsigned int bits = 0;
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (held & MAP[i].button) {
            bits |= MAP[i].osk;
        }
    }
    return bits;
}

static CarafeFailure failureOf(Kind kind) {
    switch (kind) {
    case KindMemory:
        return CarafeKindMemory;
    case KindThreads:
        return CarafeKindThreads;
    case KindCrash:
    case KindSystem:
        return CarafeKindCrash;
    default:
        return CarafeKindNone;
    }
}

static bool settingsLoop(Framebuffer* framebuffer, CarafeImage* picture, PadState* pad, const Report* report) {
    if (!carafeMenuOpenError(failureOf(report->kind))) {
        return false;
    }
    unsigned int drawn = 0;
    bool restart = false;
    while (appletMainLoop()) {
        padUpdate(pad);
        u64 held = padGetButtons(pad);
        HidAnalogStickState stick = padGetStickPos(pad, 0);
        unsigned int bits = oskButtons(held);
        if (stick.y > 16000) {
            bits |= OSK_UP;
        } else if (stick.y < -16000) {
            bits |= OSK_DOWN;
        }
        if (stick.x > 16000) {
            bits |= OSK_RIGHT;
        } else if (stick.x < -16000) {
            bits |= OSK_LEFT;
        }
        HidTouchScreenState touch = { 0 };
        bool touched = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
        CarafeMenuAction action = carafeMenuInput(bits, touched, touched ? (int)touch.touches[0].x : 0,
            touched ? (int)touch.touches[0].y : 0, armTicksToNs(armGetSystemTick()));
        if (action == CarafeMenuBack) {
            break;
        }
        if (action == CarafeMenuRestart) {
            restart = true;
            break;
        }
        unsigned int generation = carafeMenuGeneration();
        if (generation != drawn) {
            carafeMenuRender(picture);
            drawn = generation;
        }
        present(framebuffer, picture);
    }
    carafeMenuClose();
    return restart;
}

static Choice waitForChoice(Framebuffer* framebuffer, CarafeImage* picture, const Report* report) {
    Button buttons[MAX_BUTTONS + 1];
    int count = layoutButtons(report, buttons);
    int focus = 0;
    int drawn = -1;
    bool touching = true;
    bool notified = false;
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    char* initial_changes = calloc(1, CHANGES_SIZE);
    if (initial_changes != NULL) {
        carafeReadChangesText(initial_changes, CHANGES_SIZE);
    }

    Choice result = ChoiceExit;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        Choice chosen = ChoiceNone;
        if (down & HidNpadButton_A) {
            chosen = buttons[focus].choice;
        }
        if (down & (HidNpadButton_B | HidNpadButton_Plus)) {
            result = ChoiceExit;
            break;
        }
        if ((down & HidNpadButton_AnyLeft) && focus > 0) {
            focus--;
        }
        if ((down & HidNpadButton_AnyRight) && focus < count - 1) {
            focus++;
        }
        HidTouchScreenState touch = { 0 };
        bool touched = hidGetTouchScreenStates(&touch, 1) && touch.count > 0;
        if (touched && !touching) {
            int hit = buttonAt(buttons, count, (int)touch.touches[0].x, (int)touch.touches[0].y);
            if (hit >= 0) {
                chosen = buttons[hit].choice;
            }
        }
        touching = touched;
        if (chosen == ChoiceSettings) {
            if (settingsLoop(framebuffer, picture, &pad, report)) {
                result = ChoiceRestart;
                break;
            }
            drawn = -1;
            touching = true;
            continue;
        }
        if (chosen != ChoiceNone) {
            result = chosen;
            break;
        }
        if (drawn != focus) {
            render(picture, report, buttons, count, focus);
            drawn = focus;
        }
        present(framebuffer, picture);
        if (!notified) {
            appletNotifyRunning(NULL);
            notified = true;
        }
    }

    if (result == ChoiceRestart && initial_changes != NULL && carafeChangesExist()) {
        char* after = calloc(1, CHANGES_SIZE);
        if (after != NULL) {
            carafeReadChangesText(after, CHANGES_SIZE);
            if (strcmp(initial_changes, after) != 0) {
                carafeTrialWrite(true);
            }
            free(after);
        }
    }
    free(initial_changes);
    return result;
}

static void restartGame(void) {
    char target[512] = "";
    char own_nro[512] = RUNTIME_NRO;

    if (carafeOverlayProgramTarget != NULL) {
        carafeOverlayProgramTarget(target, sizeof(target));
    }
    if (carafeOverlayOwnNro != NULL) {
        char nro[512];
        if (carafeOverlayOwnNro(nro, sizeof(nro))) {
            snprintf(own_nro, sizeof(own_nro), "%s", nro);
        }
    }

    bool pushed = target[0] != '\0' && pushRunNext(target);
    carafeOverlayCommit("before starting the game again");
    if (envHasNextLoad() && own_nro[0] != '\0' && R_SUCCEEDED(envSetNextLoad(own_nro, own_nro))) {
        return;
    }
    if (R_SUCCEEDED(appletRestartProgram(NULL, 0))) {
        __nx_applet_exit_mode = 1;
        return;
    }
    if (pushed) {
        popRunNextIfMatches(target);
    }
    carafeOverlayLog("error screen: the game cannot be started again; going back to the menu");
    __nx_applet_exit_mode = 1;
}

static void exitToLauncherOrMenu(void) {
    char own_nro[512] = RUNTIME_NRO;
    bool forwarded = false;
    bool reopen_launcher = true;

    if (carafeOverlayOwnNro != NULL) {
        char nro[512];
        if (carafeOverlayOwnNro(nro, sizeof(nro))) {
            snprintf(own_nro, sizeof(own_nro), "%s", nro);
        }
    }
    if (carafeOverlayIsForwarded != NULL) {
        forwarded = carafeOverlayIsForwarded();
    }
    if (carafeOverlayReopenLauncher != NULL) {
        reopen_launcher = carafeOverlayReopenLauncher();
    }

    if (!forwarded && reopen_launcher && envHasNextLoad() && own_nro[0] != '\0'
        && R_SUCCEEDED(envSetNextLoad(own_nro, own_nro))) {
        return;
    }
    __nx_applet_exit_mode = 1;
}

static void bringBack(void) {
    char* text = malloc(CHANGES_SIZE);
    if (text != NULL && carafeTrialRead(text, CHANGES_SIZE)) {
        carafeWriteChangesText(text);
        carafeOverlayLog("error screen: the settings from before the trial run are back");
    }
    free(text);
    carafeRegistryBringBack();
    carafeTrialClear();
}

static int errorScreen(void) {
    Report report;
    char path[PATH_SIZE];
    reportPath(path, sizeof(path));
    bool reported = readReport(&report);
    remove(path);
    if (!reported) {
        carafeOverlayLog("error screen: no report; going back to the menu");
        exitToLauncherOrMenu();
        return 0;
    }

    g_strings = STRINGS[carafeLanguage()];
    loadTitle();
    carafeLoadIcon(&g_icon);

    char line[LINE_SIZE * 2];
    snprintf(line, sizeof(line), "error screen: kind %d, code 0x%08x, module %s%s", (int)report.kind, report.code,
        report.module[0] != '\0' ? report.module : "-", report.trial ? ", after a trial run" : "");
    carafeOverlayLog(line);

    Framebuffer framebuffer;
    if (R_FAILED(framebufferCreate(&framebuffer, nwindowGetDefault(), SCREEN_WIDTH, SCREEN_HEIGHT,
            PIXEL_FORMAT_RGBA_8888, 2))) {
        carafeOverlayLog("error screen: no framebuffer; going back to the menu");
        exitToLauncherOrMenu();
        return 0;
    }
    framebufferMakeLinear(&framebuffer);
    CarafeImage picture = { malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 4), SCREEN_WIDTH, SCREEN_HEIGHT };
    Choice choice = ChoiceExit;
    if (picture.pixels != NULL) {
        choice = waitForChoice(&framebuffer, &picture, &report);
        free(picture.pixels);
    }
    framebufferClose(&framebuffer);

    switch (choice) {
    case ChoiceRestart:
        carafeOverlayLog("error screen: restart");
        restartGame();
        break;
    case ChoiceRevert:
        bringBack();
        restartGame();
        break;
    case ChoiceKeep:
        carafeOverlayLog("error screen: the new settings stay");
        carafeTrialClear();
        restartGame();
        break;
    case ChoiceRecommended:
        carafeOverlayLog("error screen: recommended settings");
        carafeResetAll(NULL, false);
        carafeFailuresClear();
        restartGame();
        break;
    default:
        carafeOverlayLog("error screen: exit");
        exitToLauncherOrMenu();
        break;
    }
    return 0;
}

int __wrap_main(int argc, char** argv) {
    bool flag = (argc > 1 && argv[1] != NULL && strcmp(argv[1], ERROR_FLAG) == 0)
        || (argc > 0 && argv[0] != NULL && strcmp(argv[0], ERROR_FLAG) == 0);
    bool restart_pending = false;
    bool has_pending = readPendingMeta(&restart_pending);

    if (!flag && !(has_pending && restart_pending)) {
        if (has_pending) {
            remove(PENDING_ERROR_FILE);
        }
        return __real_main(argc, argv);
    }

    remove(PENDING_ERROR_FILE);
    g_error_mode = true;
    if (wine_nx_runtime_open_error_log != NULL) {
        wine_nx_runtime_open_error_log();
    }
    if (carafeSettingsPrepareError != NULL) {
        carafeSettingsPrepareError();
    } else if (carafeSettingsSyncProgram != NULL) {
        carafeSettingsSyncProgram();
    }
    if (carafeRegistrySyncProgram != NULL) {
        carafeRegistrySyncProgram();
    }
    int ret = errorScreen();
    carafeOverlayCommit("after error screen");
    return ret;
}
