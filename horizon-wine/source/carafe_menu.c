#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <switch.h>

#include "carafe_menu.h"
#include "carafe_overlay.h"
#include "carafe_registry.h"
#include "osk.h"

#define MENU_OPACITY 214
#define LIST_X 72
#define LIST_W 640
#define LIST_TOP 112
#define LIST_TOP_PAGES 152
#define ROW_H 42
#define ROW_GAP 2
#define LIST_BOTTOM 584
#define CARD_X 768
#define CARD_W 440
#define BUTTONS_Y 600
#define BUTTON_H 60
#define MAX_ITEMS 128
#define STRIP_MAX 4
#define MAX_HITS 160
#define REPEAT_FIRST_NS 380000000ULL
#define REPEAT_NEXT_NS 110000000ULL
#define TEXT_SIZE 256
#define CHORD_BUTTONS (HidNpadButton_Minus | HidNpadButton_Plus | HidNpadButton_L | HidNpadButton_R)
#define REG_MAX_ENTRIES 192
#define REG_LIST_TOP 194
#define REG_TEXT_MAX 200
#define REG_EDIT_W 880
#define REG_LINE 1024

typedef enum {
    ItemSetting,
    ItemGroup,
    ItemPreset,
    ItemReset,
    ItemSuggest,
    ItemRegistry,
} ItemKind;

typedef struct {
    ItemKind kind;
    int index;
    int value;
} Item;

typedef enum {
    HitTab,
    HitRow,
    HitPage,
    HitButton,
    HitStrip,
    HitSource,
    HitKey,
    HitRegChip,
    HitRegRow,
    HitRegKey,
    HitRegField,
} HitKind;

typedef enum {
    ButtonRestart,
    ButtonGame,
    ButtonBack,
    ButtonDone,
    ButtonClear,
    ButtonConfirm,
    ButtonLater,
    ButtonUndo,
    ButtonTrial,
    ButtonEditDone,
    ButtonEditCancel,
} ButtonId;

typedef struct {
    HitKind kind;
    int value;
    int x;
    int y;
    int w;
    int h;
} Hit;

typedef struct {
    const char* back;
    const char* restart;
    const char* restart_count;
    const char* to_game;
    const char* to_error;
    const char* change;
    const char* reset;
    const char* open;
    const char* sections;
    const char* pages;
    const char* now;
    const char* after_restart;
    const char* after_restart_error;
    const char* inactive;
    const char* default_mark;
    const char* preset;
    const char* custom;
    const char* preset_hint;
    const char* reset_all;
    const char* reset_all_hint;
    const char* reset_again;
    const char* confirm_title;
    const char* confirm_body;
    const char* trial;
    const char* trial_hint;
    const char* later;
    const char* undo;
    const char* pending[3];
    const char* running;
    const char* failed;
    const char* what_to_try;
    const char* button;
    const char* done;
    const char* clear;
    const char* pick_keys;
    const char* pick_pad;
    const char* toggle;
    const char* choose;
    const char* cancel;
    const char* live_controls;
    const char* gamepad;
    const char* keyboard;
    const char* rows[10][2];
    const char* stick_press;
    const char* none;
    const char* trial_toggle;
} Strings;

static const Strings STRINGS[] = {
    {
        "Назад", "Перезапустить", "Перезапустить · %d", "В игру", "К ошибке", "Изменить", "Сбросить", "Открыть",
        "Разделы", "Страницы", "Действует сразу — видно в игре.", "Применится после перезапуска игры.",
        "Применится, когда игра запустится снова.", "Сейчас не действует.", "по умолчанию", "Набор", "Свой",
        "Готовые сочетания опций. Можно выбрать набор и поправить опции вручную — тогда набор станет «Свой».",
        "Вернуть рекомендованные", "Все настройки этой игры станут такими, как их собрал Carafe.",
        "Нажмите A ещё раз, чтобы вернуть", "Перезапустить игру?",
        "Эти настройки начнут действовать с нового запуска. Прогресс, который игра не сохранила, пропадёт.",
        "Пробный запуск", "Если игра упадёт с новыми настройками — предложить вернуть прежние.", "Позже", "Отменить",
        { "%d изменение ждёт перезапуска", "%d изменения ждут перезапуска", "%d изменений ждут перезапуска" },
        "Игра идёт под меню · закрыть: −, +, L и R вместе", "Игра закрылась из-за ошибки", "Что попробовать",
        "Кнопка %s", "Готово", "Очистить", "Выберите клавиши и кнопки мыши. Несколько — получится сочетание.",
        "Какой кнопкой геймпада Xbox станет эта кнопка Switch.", "Добавить или убрать", "Выбрать", "Отмена",
        "Действует сразу — проверьте в игре.", "геймпад Xbox 360", "клавиатура и мышь",
        {
            { "ZL", "курок" }, { "L", "плечо" }, { "−", "минус" }, { "Левый стик", "наклон и нажатие" },
            { "Крестовина", "четыре кнопки" }, { "ZR", "курок" }, { "R", "плечо" }, { "+", "плюс" },
            { "A B X Y", "кнопки действия" }, { "Правый стик", "наклон и нажатие" },
        },
        "нажатие", "Нет", "Пробный запуск",
    },
    {
        "Back", "Restart", "Restart · %d", "Back to game", "Back", "Change", "Reset", "Open", "Sections", "Pages",
        "Takes effect now — you can see it in the game.", "Takes effect when the game restarts.",
        "Takes effect when the game starts again.", "Has no effect right now.", "default", "Preset", "Custom",
        "Ready-made sets of options. Pick one and adjust the options by hand — the preset then shows as Custom.",
        "Restore recommended", "Every setting of this game goes back to how Carafe built it.",
        "Press A again to restore", "Restart the game?",
        "These settings take effect on the next start. Progress the game has not saved will be lost.",
        "Trial run", "If the game crashes with the new settings, offer to bring back the old ones.", "Later", "Undo",
        { "%d change waits for a restart", "%d changes wait for a restart", "%d changes wait for a restart" },
        "The game keeps running · close: −, +, L and R together", "The game closed with an error", "What to try",
        "Button %s", "Done", "Clear", "Choose keys and mouse buttons. Several make a combination.",
        "Which Xbox controller button this Switch button becomes.", "Add or remove", "Choose", "Cancel",
        "Takes effect now — try it in the game.", "Xbox 360 controller", "keyboard and mouse",
        {
            { "ZL", "trigger" }, { "L", "shoulder" }, { "−", "minus" }, { "Left stick", "tilt and press" },
            { "D-pad", "four buttons" }, { "ZR", "trigger" }, { "R", "shoulder" }, { "+", "plus" },
            { "A B X Y", "action buttons" }, { "Right stick", "tilt and press" },
        },
        "press", "None", "Trial run",
    },
    {
        "返回", "重新启动", "重新启动 · %d", "回到游戏", "返回", "更改", "重置", "打开", "分区", "页面",
        "立即生效——可在游戏中看到。", "重新启动游戏后生效。", "游戏再次启动时生效。", "当前不起作用。", "默认", "预设",
        "自定义", "现成的选项组合。可以选择预设后再手动调整，此时预设显示为“自定义”。", "恢复推荐设置",
        "此游戏的所有设置恢复为 Carafe 构建时的样子。", "再按一次 A 以恢复", "重新启动游戏？",
        "这些设置将在下次启动时生效。游戏未保存的进度将会丢失。", "试运行", "如果游戏在新设置下崩溃，提供恢复旧设置的选项。",
        "稍后", "撤销", { "%d 项更改等待重新启动", "%d 项更改等待重新启动", "%d 项更改等待重新启动" },
        "游戏仍在运行 · 关闭：同时按 −、+、L 和 R", "游戏因错误而关闭", "可以尝试", "按钮 %s", "完成", "清除",
        "选择按键和鼠标按钮。选择多个即组成组合键。", "这个 Switch 按钮对应 Xbox 手柄的哪个按钮。", "添加或移除", "选择",
        "取消", "立即生效——请在游戏中试试。", "Xbox 360 手柄", "键盘和鼠标",
        {
            { "ZL", "扳机" }, { "L", "肩键" }, { "−", "减号" }, { "左摇杆", "推动与按下" }, { "十字键", "四个按钮" },
            { "ZR", "扳机" }, { "R", "肩键" }, { "+", "加号" }, { "A B X Y", "动作按钮" }, { "右摇杆", "推动与按下" },
        },
        "按下", "无", "试运行",
    },
    {
        "もどる", "再起動", "再起動 · %d", "ゲームに戻る", "もどる", "変更", "リセット", "開く", "カテゴリー", "ページ",
        "すぐに反映されます。ゲームで確認できます。", "ゲームを再起動すると反映されます。",
        "ゲームを次に起動したときに反映されます。", "今は効果がありません。", "デフォルト", "プリセット", "カスタム",
        "オプションの組み合わせです。選んだ後にオプションを手動で調整すると、プリセットは「カスタム」になります。",
        "おすすめに戻す", "このゲームのすべての設定が、Carafeがビルドしたときの状態に戻ります。",
        "もう一度Aを押すと戻します", "ゲームを再起動しますか？",
        "これらの設定は次の起動から反映されます。ゲームで保存していない進行状況は失われます。",
        "お試し起動", "新しい設定でゲームがクラッシュしたら、元の設定に戻すか確認します。", "あとで", "元に戻す",
        { "%d件の変更が再起動待ちです", "%d件の変更が再起動待ちです", "%d件の変更が再起動待ちです" },
        "ゲームは動作中 · 閉じる：−、+、L、Rを同時に押す", "ゲームはエラーで終了しました", "試せること",
        "%sボタン", "完了", "クリア", "キーとマウスボタンを選んでください。複数選ぶと組み合わせになります。",
        "このSwitchのボタンを、Xboxコントローラーのどのボタンにするかを選びます。", "追加/削除", "決定", "キャンセル",
        "すぐに反映されます。ゲームで試してください。", "Xbox 360コントローラー", "キーボードとマウス",
        {
            { "ZL", "トリガー" }, { "L", "ショルダー" }, { "−", "マイナス" }, { "Lスティック", "傾け・押し込み" },
            { "方向ボタン", "4つのボタン" }, { "ZR", "トリガー" }, { "R", "ショルダー" }, { "+", "プラス" },
            { "A B X Y", "アクションボタン" }, { "Rスティック", "傾け・押し込み" },
        },
        "押し込み", "なし", "お試し起動",
    },
    {
        "뒤로", "다시 시작", "다시 시작 · %d", "게임으로", "뒤로", "변경", "초기화", "열기", "분류", "페이지",
        "바로 적용됩니다. 게임에서 확인할 수 있습니다.", "게임을 다시 시작하면 적용됩니다.",
        "게임을 다시 실행하면 적용됩니다.", "지금은 효과가 없습니다.", "기본값", "프리셋", "사용자 지정",
        "미리 만든 옵션 조합입니다. 하나를 고른 뒤 옵션을 직접 조정하면 프리셋이 “사용자 지정”으로 표시됩니다.",
        "권장 설정으로", "이 게임의 모든 설정이 Carafe가 빌드한 상태로 돌아갑니다.",
        "되돌리려면 A를 한 번 더 누르세요", "게임을 다시 시작할까요?",
        "이 설정은 다음 실행부터 적용됩니다. 게임에서 저장하지 않은 진행 상황은 사라집니다.",
        "시험 실행", "새 설정으로 게임이 오류로 종료되면 이전 설정으로 되돌릴지 묻습니다.", "나중에", "되돌리기",
        { "변경 %d개가 다시 시작을 기다립니다", "변경 %d개가 다시 시작을 기다립니다",
            "변경 %d개가 다시 시작을 기다립니다" },
        "게임은 계속 실행 중 · 닫기: −, +, L, R 동시에 누르기", "게임이 오류로 종료되었습니다", "해결 방법",
        "%s 버튼", "완료", "지우기", "키와 마우스 버튼을 고르세요. 여러 개를 고르면 조합이 됩니다.",
        "이 Switch 버튼이 Xbox 컨트롤러의 어떤 버튼이 될지 고릅니다.", "추가/제거", "선택", "취소",
        "바로 적용됩니다. 게임에서 확인해 보세요.", "Xbox 360 컨트롤러", "키보드와 마우스",
        {
            { "ZL", "트리거" }, { "L", "숄더" }, { "−", "마이너스" }, { "L스틱", "기울이기·누르기" },
            { "방향 버튼", "버튼 4개" }, { "ZR", "트리거" }, { "R", "숄더" }, { "+", "플러스" },
            { "A B X Y", "액션 버튼" }, { "R스틱", "기울이기·누르기" },
        },
        "누르기", "없음", "시험 실행",
    },
    {
        "返回", "重新啟動", "重新啟動 · %d", "回到遊戲", "返回", "變更", "重設", "開啟", "分類", "頁面",
        "立即生效，可在遊戲中看到。", "重新啟動遊戲後生效。", "遊戲再次啟動時生效。", "目前沒有作用。", "預設", "方案",
        "自訂", "現成的選項組合。選擇一個後可手動調整選項，此時方案會顯示為「自訂」。", "恢復建議設定",
        "這款遊戲的所有設定都會恢復為 Carafe 建置時的樣子。", "再按一次 A 以恢復", "要重新啟動遊戲嗎？",
        "這些設定會在下次啟動時生效。遊戲尚未儲存的進度將會遺失。", "試執行", "如果遊戲在新設定下當機，提議恢復原本的設定。",
        "稍後", "復原", { "%d 項變更等待重新啟動", "%d 項變更等待重新啟動", "%d 項變更等待重新啟動" },
        "遊戲仍在執行 · 關閉：同時按 −、+、L 和 R", "遊戲因錯誤而關閉", "可以試試", "按鈕 %s", "完成", "清除",
        "選擇鍵盤按鍵與滑鼠按鈕。選擇多個即成為組合鍵。", "這個 Switch 按鈕要對應 Xbox 控制器的哪個按鈕。", "新增或移除",
        "選擇", "取消", "立即生效，請在遊戲中試試。", "Xbox 360 控制器", "鍵盤與滑鼠",
        {
            { "ZL", "扳機" }, { "L", "肩鍵" }, { "−", "減號" }, { "左搖桿", "傾斜與按下" }, { "方向按鈕", "四個按鈕" },
            { "ZR", "扳機" }, { "R", "肩鍵" }, { "+", "加號" }, { "A B X Y", "動作按鈕" }, { "右搖桿", "傾斜與按下" },
        },
        "按下", "無", "試執行",
    },
    {
        "Retour", "Redémarrer", "Redémarrer · %d", "Reprendre", "Retour", "Modifier", "Rétablir", "Ouvrir",
        "Sections", "Pages", "Effet immédiat — visible dans le jeu.", "Prend effet au redémarrage du jeu.",
        "Prend effet au prochain lancement du jeu.", "Sans effet pour le moment.", "par défaut", "Préréglage",
        "Personnalisé",
        "Des combinaisons d’options toutes prêtes. Choisissez-en une et ajustez les options à la main — le "
        "préréglage devient alors « Personnalisé ».",
        "Rétablir les recommandations",
        "Tous les paramètres de ce jeu reviennent à ceux avec lesquels Carafe l’a généré.",
        "Appuyez encore sur A pour rétablir", "Redémarrer le jeu ?",
        "Ces paramètres prendront effet au prochain lancement. La progression non sauvegardée par le jeu sera perdue.",
        "Lancement d’essai", "Si le jeu plante avec les nouveaux paramètres, proposer de revenir aux anciens.",
        "Plus tard", "Annuler",
        { "%d changement attend un redémarrage", "%d changements attendent un redémarrage",
            "%d changements attendent un redémarrage" },
        "Le jeu continue de tourner · fermer : −, +, L et R ensemble", "Le jeu s’est fermé à cause d’une erreur",
        "À essayer", "Bouton %s", "OK", "Vider",
        "Choisissez des touches et des boutons de souris. Plusieurs forment une combinaison.",
        "Le bouton de manette Xbox que devient ce bouton Switch.", "Ajouter ou retirer", "Choisir", "Annuler",
        "Effet immédiat — essayez dans le jeu.", "manette Xbox 360", "clavier et souris",
        {
            { "ZL", "gâchette" }, { "L", "tranche" }, { "−", "moins" }, { "Stick gauche", "incliner, clic" },
            { "Croix", "quatre boutons" }, { "ZR", "gâchette" }, { "R", "tranche" }, { "+", "plus" },
            { "A B X Y", "boutons action" }, { "Stick droit", "incliner, clic" },
        },
        "appui", "Aucun", "Lancement d’essai",
    },
    {
        "Retour", "Redémarrer", "Redémarrer · %d", "Reprendre", "Retour", "Modifier", "Rétablir", "Ouvrir",
        "Sections", "Pages", "S’applique tout de suite — visible dans le jeu.", "S’applique au redémarrage du jeu.",
        "S’applique au prochain démarrage du jeu.", "Sans effet pour l’instant.", "par défaut", "Préréglage",
        "Personnalisé",
        "Ensembles d’options prêts à l’emploi. Choisissez-en un et ajustez les options à la main — le préréglage "
        "devient alors « Personnalisé ».",
        "Rétablir les réglages recommandés",
        "Tous les paramètres de ce jeu reviennent à ceux de sa création par Carafe.",
        "Appuyez de nouveau sur A pour rétablir", "Redémarrer le jeu?",
        "Ces paramètres s’appliqueront au prochain démarrage. Toute progression non sauvegardée sera perdue.",
        "Lancement d’essai", "Si le jeu plante avec les nouveaux paramètres, proposer de revenir aux anciens.",
        "Plus tard", "Défaire",
        { "%d changement attend un redémarrage", "%d changements attendent un redémarrage",
            "%d changements attendent un redémarrage" },
        "Le jeu continue de tourner · fermer : −, +, L et R ensemble", "Le jeu s’est fermé à cause d’une erreur",
        "À essayer", "Bouton %s", "Terminé", "Effacer",
        "Choisissez des touches et des boutons de souris. Plusieurs forment une combinaison.",
        "Le bouton de manette Xbox que devient ce bouton de la Switch.", "Ajouter ou retirer", "Choisir", "Annuler",
        "S’applique tout de suite — essayez dans le jeu.", "manette Xbox 360", "clavier et souris",
        {
            { "ZL", "gâchette" }, { "L", "épaule" }, { "−", "moins" }, { "Joystick gauche", "incliner, appuyer" },
            { "Directions", "quatre boutons" }, { "ZR", "gâchette" }, { "R", "épaule" }, { "+", "plus" },
            { "A B X Y", "boutons d’action" }, { "Joystick droit", "incliner, appuyer" },
        },
        "appui", "Aucun", "Lancement d’essai",
    },
    {
        "Zurück", "Neustart", "Neustart · %d", "Zum Spiel", "Zurück", "Ändern", "Zurücksetzen", "Öffnen", "Bereiche",
        "Seiten", "Wirkt sofort – im Spiel sichtbar.", "Wirkt nach einem Neustart des Spiels.",
        "Wirkt, wenn das Spiel wieder startet.", "Wirkt gerade nicht.", "Standard", "Voreinstellung", "Eigene",
        "Fertige Optionssätze. Wähle einen und passe die Optionen von Hand an – die Voreinstellung heißt dann "
        "„Eigene“.",
        "Empfohlene wiederherstellen", "Alle Einstellungen dieses Spiels werden so, wie Carafe sie erstellt hat.",
        "Drücke erneut A zum Wiederherstellen", "Spiel neu starten?",
        "Diese Einstellungen wirken ab dem nächsten Start. Nicht gespeicherter Fortschritt geht verloren.",
        "Probelauf", "Wenn das Spiel mit den neuen Einstellungen abstürzt, anbieten, die alten zurückzuholen.",
        "Später", "Rückgängig",
        { "%d Änderung wartet auf Neustart", "%d Änderungen warten auf Neustart", "%d Änderungen warten auf Neustart" },
        "Das Spiel läuft weiter · schließen: −, +, L und R zusammen", "Das Spiel wurde wegen eines Fehlers beendet",
        "Was du probieren kannst", "Taste %s", "Fertig", "Leeren",
        "Wähle Tasten und Maustasten. Mehrere ergeben eine Kombination.",
        "Welche Taste des Xbox-Controllers diese Switch-Taste wird.", "Hinzufügen/entfernen", "Wählen", "Abbrechen",
        "Wirkt sofort – probier es im Spiel aus.", "Xbox-360-Controller", "Tastatur und Maus",
        {
            { "ZL", "Trigger" }, { "L", "Schultertaste" }, { "−", "Minus" }, { "Linker Stick", "neigen/drücken" },
            { "Steuerkreuz", "vier Tasten" }, { "ZR", "Trigger" }, { "R", "Schultertaste" }, { "+", "Plus" },
            { "A B X Y", "Aktionstasten" }, { "Rechter Stick", "neigen/drücken" },
        },
        "drücken", "Keine", "Probelauf",
    },
    {
        "Indietro", "Riavvia", "Riavvia · %d", "Al gioco", "Indietro", "Cambia", "Ripristina", "Apri", "Sezioni",
        "Pagine", "Ha effetto subito — si vede nel gioco.", "Ha effetto al riavvio del gioco.",
        "Ha effetto al prossimo avvio del gioco.", "Ora non ha effetto.", "predefinito", "Preset", "Personalizzato",
        "Combinazioni di opzioni pronte. Scegline una e regola le opzioni a mano: il preset diventerà Personalizzato.",
        "Ripristina consigliate", "Tutte le impostazioni di questo gioco tornano come quando Carafe lo ha compilato.",
        "Premi di nuovo A per ripristinare", "Riavviare il gioco?",
        "Queste impostazioni avranno effetto al prossimo avvio. I progressi non salvati dal gioco andranno persi.",
        "Avvio di prova", "Se il gioco va in crash con le nuove impostazioni, propone di tornare a quelle precedenti.",
        "Dopo", "Annulla",
        { "%d modifica attende il riavvio", "%d modifiche attendono il riavvio", "%d modifiche attendono il riavvio" },
        "Il gioco continua · per chiudere: −, +, L e R insieme", "Il gioco si è chiuso per un errore", "Cosa provare",
        "Pulsante %s", "Fine", "Cancella", "Scegli tasti e pulsanti del mouse. Più di uno formano una combinazione.",
        "Quale pulsante del controller Xbox diventa questo pulsante della Switch.", "Aggiungi o rimuovi", "Scegli",
        "Annulla", "Ha effetto subito — provalo nel gioco.", "controller Xbox 360", "tastiera e mouse",
        {
            { "ZL", "grilletto" }, { "L", "dorsale" }, { "−", "meno" }, { "Stick sinistro", "inclina, premi" },
            { "Croce", "quattro pulsanti" }, { "ZR", "grilletto" }, { "R", "dorsale" }, { "+", "più" },
            { "A B X Y", "tasti azione" }, { "Stick destro", "inclina, premi" },
        },
        "pressione", "Nessuno", "Avvio di prova",
    },
    {
        "Atrás", "Reiniciar", "Reiniciar · %d", "Al juego", "Volver", "Cambiar", "Restablecer", "Abrir", "Secciones",
        "Páginas", "Se aplica al momento: puedes verlo en el juego.", "Se aplicará cuando reinicies el juego.",
        "Se aplicará cuando el juego vuelva a iniciarse.", "Ahora mismo no tiene efecto.", "por defecto", "Preajuste",
        "Personalizado",
        "Combinaciones de opciones ya listas. Elige una y ajusta las opciones a mano: el preajuste pasará a "
        "«Personalizado».",
        "Volver a los recomendados", "Todos los ajustes de este juego vuelven a como los dejó Carafe al generarlo.",
        "Pulsa A otra vez para restablecer", "¿Reiniciar el juego?",
        "Estos ajustes se aplicarán la próxima vez que se inicie. Se perderá el progreso que el juego no haya "
        "guardado.",
        "Modo de prueba", "Si el juego falla con los ajustes nuevos, ofrece recuperar los anteriores.", "Más tarde",
        "Deshacer",
        { "%d cambio pendiente de reinicio", "%d cambios pendientes de reinicio", "%d cambios pendientes de reinicio" },
        "El juego sigue abierto · cerrar: −, +, L y R a la vez", "El juego se ha cerrado por un error",
        "Qué puedes probar", "Botón %s", "Listo", "Borrar",
        "Elige teclas y botones del ratón. Si eliges varios, forman una combinación.",
        "Qué botón del mando de Xbox será este botón de la Switch.", "Añadir o quitar", "Elegir", "Cancelar",
        "Se aplica al momento: pruébalo en el juego.", "mando de Xbox 360", "teclado y ratón",
        {
            { "ZL", "gatillo" }, { "L", "botón superior" }, { "−", "menos" }, { "Palanca izq.", "mover y pulsar" },
            { "Cruceta", "cuatro botones" }, { "ZR", "gatillo" }, { "R", "botón superior" }, { "+", "más" },
            { "A B X Y", "acción" }, { "Palanca der.", "mover y pulsar" },
        },
        "pulsar", "Ninguno", "Modo de prueba",
    },
    {
        "Atrás", "Reiniciar", "Reiniciar · %d", "Volver al juego", "Volver", "Cambiar", "Restablecer", "Abrir",
        "Secciones", "Páginas", "Se aplica ahora: puedes verlo en el juego.", "Se aplica al reiniciar el juego.",
        "Se aplica cuando el juego vuelva a iniciarse.", "Ahora no tiene efecto.", "predeterminado", "Preajuste",
        "Personalizado",
        "Combinaciones de opciones listas. Elige una y ajusta las opciones a mano: el preajuste se mostrará como "
        "Personalizado.",
        "Restaurar lo recomendado", "Toda la configuración de este juego vuelve a como la generó Carafe.",
        "Presiona A otra vez para restaurar", "¿Reiniciar el juego?",
        "Esta configuración se aplicará en el próximo inicio. Se perderá el progreso que el juego no haya guardado.",
        "Inicio de prueba", "Si el juego falla con la nueva configuración, ofrecer volver a la anterior.", "Más tarde",
        "Deshacer",
        { "%d cambio espera un reinicio", "%d cambios esperan un reinicio", "%d cambios esperan un reinicio" },
        "El juego sigue en marcha · cerrar: −, +, L y R a la vez", "El juego se cerró por un error", "Qué probar",
        "Botón %s", "Listo", "Borrar", "Elige teclas y botones del mouse. Si eliges varios, se forma una combinación.",
        "Qué botón del control de Xbox será este botón de Switch.", "Agregar o quitar", "Elegir", "Cancelar",
        "Se aplica ahora: pruébalo en el juego.", "control de Xbox 360", "teclado y mouse",
        {
            { "ZL", "gatillo" }, { "L", "botón superior" }, { "−", "menos" }, { "Palanca izq.", "mover y presionar" },
            { "Direcciones", "cuatro botones" }, { "ZR", "gatillo" }, { "R", "botón superior" }, { "+", "más" },
            { "A B X Y", "botones de acción" }, { "Palanca der.", "mover y presionar" },
        },
        "presionar", "Ninguno", "Inicio de prueba",
    },
    {
        "Terug", "Herstarten", "Herstarten · %d", "Naar game", "Terug", "Wijzigen", "Resetten", "Openen", "Onderdelen",
        "Pagina's", "Werkt meteen — je ziet het in de game.", "Werkt na een herstart van de game.",
        "Werkt als de game opnieuw start.", "Heeft nu geen effect.", "standaard", "Voorinstelling", "Aangepast",
        "Kant-en-klare sets opties. Kies er een en pas de opties met de hand aan — de voorinstelling heet dan "
        "Aangepast.",
        "Aanbevolen herstellen", "Alle instellingen van deze game gaan terug naar hoe Carafe hem heeft gebouwd.",
        "Druk nogmaals op A om te herstellen", "Game herstarten?",
        "Deze instellingen werken vanaf de volgende start. Voortgang die de game niet heeft opgeslagen, gaat verloren.",
        "Proefstart", "Crasht de game met de nieuwe instellingen, bied dan aan de oude terug te zetten.", "Later",
        "Ongedaan maken",
        { "%d wijziging wacht op een herstart", "%d wijzigingen wachten op een herstart",
            "%d wijzigingen wachten op een herstart" },
        "De game blijft draaien · sluiten: −, +, L en R tegelijk", "De game is afgesloten door een fout",
        "Wat je kunt proberen", "Knop %s", "Klaar", "Wissen",
        "Kies toetsen en muisknoppen. Meerdere vormen samen een combinatie.",
        "Welke knop van de Xbox-controller deze Switch-knop wordt.", "Toevoegen of verwijderen", "Kiezen", "Annuleren",
        "Werkt meteen — probeer het in de game.", "Xbox 360-controller", "toetsenbord en muis",
        {
            { "ZL", "trigger" }, { "L", "schouderknop" }, { "−", "min" }, { "Linkerstick", "kantelen en indrukken" },
            { "Richtingsknoppen", "vier knoppen" }, { "ZR", "trigger" }, { "R", "schouderknop" }, { "+", "plus" },
            { "A B X Y", "actieknoppen" }, { "Rechterstick", "kantelen en indrukken" },
        },
        "indrukken", "Geen", "Proefstart",
    },
    {
        "Voltar", "Reiniciar", "Reiniciar · %d", "Voltar ao jogo", "Voltar", "Alterar", "Repor", "Abrir", "Secções",
        "Páginas", "Tem efeito imediato — pode vê-lo no jogo.", "Tem efeito quando o jogo reiniciar.",
        "Tem efeito quando o jogo voltar a arrancar.", "Neste momento não tem efeito.", "predefinido", "Perfil",
        "Personalizado",
        "Conjuntos de opções prontos. Escolha um e ajuste as opções manualmente — o perfil passa a aparecer como "
        "Personalizado.",
        "Repor recomendadas", "Todas as definições deste jogo voltam a ser como o Carafe as compilou.",
        "Prima A novamente para repor", "Reiniciar o jogo?",
        "Estas definições têm efeito no próximo arranque. O progresso que o jogo não guardou vai perder-se.",
        "Arranque de teste", "Se o jogo falhar com as novas definições, propor repor as antigas.", "Mais tarde",
        "Anular",
        { "%d alteração aguarda reinício", "%d alterações aguardam reinício", "%d alterações aguardam reinício" },
        "O jogo continua a correr · fechar: −, +, L e R em simultâneo", "O jogo fechou devido a um erro",
        "O que experimentar", "Botão %s", "OK", "Limpar",
        "Escolha teclas e botões do rato. Vários formam uma combinação.",
        "Em que botão do comando Xbox se torna este botão da Switch.", "Adicionar ou remover", "Escolher", "Cancelar",
        "Tem efeito imediato — experimente no jogo.", "comando Xbox 360", "teclado e rato",
        {
            { "ZL", "gatilho" }, { "L", "ombro" }, { "−", "menos" }, { "Manípulo esq.", "mover e premir" },
            { "Direcionais", "quatro botões" }, { "ZR", "gatilho" }, { "R", "ombro" }, { "+", "mais" },
            { "A B X Y", "botões de ação" }, { "Manípulo dir.", "mover e premir" },
        },
        "premir", "Nenhum", "Arranque de teste",
    },
    {
        "Voltar", "Reiniciar", "Reiniciar · %d", "Voltar ao jogo", "Voltar", "Alterar", "Redefinir", "Abrir", "Seções",
        "Páginas", "Tem efeito imediato — dá para ver no jogo.", "Tem efeito quando o jogo reiniciar.",
        "Tem efeito quando o jogo for iniciado de novo.", "Sem efeito no momento.", "padrão", "Predefinição",
        "Personalizada",
        "Conjuntos prontos de opções. Escolha um e ajuste as opções manualmente — a predefinição passa a aparecer "
        "como Personalizada.",
        "Restaurar recomendadas", "Todas as configurações deste jogo voltam a ser como o Carafe as criou.",
        "Pressione A de novo para restaurar", "Reiniciar o jogo?",
        "Estas configurações passam a valer na próxima vez que o jogo iniciar. O progresso não salvo será perdido.",
        "Modo de teste", "Se o jogo falhar com as novas configurações, oferecer a volta das anteriores.", "Depois",
        "Desfazer",
        { "%d alteração aguarda reinício", "%d alterações aguardam reinício", "%d alterações aguardam reinício" },
        "O jogo continua rodando · fechar: −, +, L e R juntos", "O jogo fechou com um erro", "O que tentar",
        "Botão %s", "Pronto", "Limpar", "Escolha teclas e botões do mouse. Vários formam uma combinação.",
        "Qual botão do controle Xbox este botão do Switch passa a ser.", "Adicionar ou remover", "Escolher",
        "Cancelar", "Tem efeito imediato — teste no jogo.", "controle Xbox 360", "teclado e mouse",
        {
            { "ZL", "gatilho" }, { "L", "ombro" }, { "−", "menos" }, { "Direcional L", "mover e clicar" },
            { "Direcionais", "quatro botões" }, { "ZR", "gatilho" }, { "R", "ombro" }, { "+", "mais" },
            { "A B X Y", "botões de ação" }, { "Direcional R", "mover e clicar" },
        },
        "clique", "Nenhum", "Modo de teste",
    },
};

_Static_assert(sizeof(STRINGS) / sizeof(STRINGS[0]) == CarafeLanguageCount, "a menu text for every language");

typedef struct {
    const char* title;
    const char* item_hint;
    const char* from_app;
    const char* whole;
    const char* computer;
    const char* hive_hint[2];
    const char* key_info;
    const char* empty;
    const char* missing;
    const char* type_names[6];
    const char* type_other;
    const char* tags[3];
    const char* was;
    const char* path;
    const char* writes;
    const char* own_value;
    const char* mode_row;
    const char* mode_value;
    const char* mode_added;
    const char* mode_deleted;
    const char* mode_readonly;
    const char* mode_key;
    const char* mode_empty;
    const char* delete_again;
    const char* open_change;
    const char* new_value;
    const char* undo;
    const char* remove;
    const char* up;
    const char* rows;
    const char* in_key;
    const char* name;
    const char* type;
    const char* value;
    const char* number;
    const char* string;
    const char* decimal;
    const char* hex;
    const char* press;
    const char* erase;
    const char* base;
    const char* field;
    const char* space;
    const char* deleted;
    const char* name_needed;
    const char* too_big;
} RegStrings;

static const RegStrings REG_STRINGS[] = {
    {
        "Реестр игры",
        "Записи реестра Windows, в которых игра хранит свои настройки: разрешение, язык, управление. Сначала — "
        "записи, подготовленные в приложении, дальше — весь реестр этой игры.",
        "Из приложения · %d", "Весь реестр", "Компьютер",
        { "Настройки системы: путь установки игры, версия, общие для всех параметры.",
            "Настройки пользователя. Здесь игры обычно хранят разрешение, язык и управление." },
        "Раздел: %d подразд. · %d знач.", "В этом разделе ничего нет.", "Раздела пока нет: игра ещё не создала его.",
        { "Число (REG_DWORD).", "Число (REG_QWORD).", "Строка (REG_SZ).", "Строка с переменными (REG_EXPAND_SZ).",
            "Список строк (REG_MULTI_SZ).", "Двоичные данные (REG_BINARY)." },
        "Данные типа %u.", { "новое", "удалено", "изменено" }, "Было: %s · Y — вернуть", "Путь:", "Пишет в реестр:",
        "Своё значение", "Применится после перезапуска игры. Запись подготовлена в приложении.",
        "A — изменить. Игра прочтёт новое значение после перезапуска.",
        "Новое значение. Появится в реестре после перезапуска.",
        "Будет удалено при перезапуске. A или Y — оставить.", "Такие данные правятся в приложении.", "A — открыть",
        "X — создать значение", "Нажмите − ещё раз, чтобы удалить", "Открыть / изменить", "Новое значение",
        "Вернуть", "Удалить", "Наверх", "Записи", "В разделе %s", "Имя", "Тип", "Значение", "Число", "Строка",
        "Десятичное", "Шестнадцатеричное", "Нажать", "Стереть", "Дес. / шестн.", "Поле", "Пробел", "удалить",
        "Введите имя значения.", "Слишком большое число.",
    },
    {
        "Game registry",
        "Windows registry entries where the game keeps its settings: resolution, language, controls. First the "
        "entries prepared in the app, then the game's whole registry.",
        "From the app · %d", "Whole registry", "Computer",
        { "System settings: where the game is installed, its version, settings shared by everyone.",
            "User settings. Games usually keep their resolution, language and controls here." },
        "Key: %d subkeys · %d values", "This key is empty.", "This key does not exist yet: the game has not made it.",
        { "Number (REG_DWORD).", "Number (REG_QWORD).", "String (REG_SZ).", "String with variables (REG_EXPAND_SZ).",
            "List of strings (REG_MULTI_SZ).", "Binary data (REG_BINARY)." },
        "Data of type %u.", { "new", "deleted", "changed" }, "Was: %s · Y — undo", "Path:", "Writes to the registry:",
        "Own value", "Takes effect when the game restarts. The entry was prepared in the app.",
        "A — change. The game reads the new value when it restarts.",
        "New value. It appears in the registry when the game restarts.",
        "Deleted when the game restarts. A or Y — keep it.", "Data of this kind is edited in the app.", "A — open",
        "X — create a value", "Press − again to delete", "Open / change", "New value", "Undo", "Delete", "Up",
        "Entries", "In %s", "Name", "Type", "Value", "Number", "String", "Decimal", "Hexadecimal", "Press", "Erase",
        "Dec / hex", "Field", "Space", "delete", "Enter a name for the value.", "The number is too big.",
    },
    {
        "游戏注册表",
        "游戏保存其设置的 Windows 注册表项：分辨率、语言、操作。先是在应用中准备的项，然后是此游戏的整个注册表。",
        "来自应用 · %d", "整个注册表", "计算机",
        { "系统设置：游戏的安装位置、版本、所有人共用的设置。", "用户设置。游戏通常在这里保存分辨率、语言和操作。" },
        "项：%d 个子项 · %d 个值", "此项为空。", "此项尚不存在：游戏还没有创建它。",
        { "数字 (REG_DWORD)。", "数字 (REG_QWORD)。", "字符串 (REG_SZ)。", "带变量的字符串 (REG_EXPAND_SZ)。",
            "字符串列表 (REG_MULTI_SZ)。", "二进制数据 (REG_BINARY)。" },
        "类型 %u 的数据。", { "新", "已删除", "已更改" }, "原值：%s · Y — 撤销", "路径：", "写入注册表：", "自定义值",
        "重新启动游戏后生效。此项是在应用中准备的。", "A — 更改。游戏重新启动后读取新值。", "新值。游戏重新启动后出现在注册表中。",
        "游戏重新启动时删除。A 或 Y — 保留。", "此类数据在应用中编辑。", "A — 打开", "X — 创建值", "再按一次 − 以删除",
        "打开 / 更改", "新值", "撤销", "删除", "上一级", "项目", "位于 %s", "名称", "类型", "值", "数字", "字符串",
        "十进制", "十六进制", "按下", "删除字符", "十 / 十六进制", "字段", "空格", "删除", "请输入值的名称。", "数字太大。",
    },
    {
        "ゲームのレジストリ",
        "ゲームが解像度、言語、操作などの設定を保存しているWindowsのレジストリのエントリーです。"
        "最初にアプリで用意したエントリー、その後にこのゲームのレジストリ全体が表示されます。",
        "アプリから · %d", "レジストリ全体", "コンピューター",
        { "システムの設定です。ゲームのインストール先やバージョン、全ユーザー共通の設定があります。",
            "ユーザーの設定です。ゲームは解像度、言語、操作をたいていここに保存します。" },
        "キー：サブキー%d個 · 値%d個", "このキーは空です。", "このキーはまだありません。ゲームがまだ作成していません。",
        { "数値（REG_DWORD）。", "数値（REG_QWORD）。", "文字列（REG_SZ）。", "変数付き文字列（REG_EXPAND_SZ）。",
            "文字列のリスト（REG_MULTI_SZ）。", "バイナリデータ（REG_BINARY）。" },
        "種類%uのデータです。", { "新規", "削除", "変更" }, "元の値：%s · Y：元に戻す", "パス：", "レジストリへの書き込み：",
        "独自の値", "ゲームを再起動すると反映されます。このエントリーはアプリで用意されました。",
        "A：変更。ゲームは再起動後に新しい値を読み込みます。",
        "新しい値です。ゲームを再起動するとレジストリに追加されます。",
        "ゲームを再起動すると削除されます。AかY：残す。", "この種類のデータはアプリで編集します。", "A：開く",
        "X：値を作成", "もう一度−を押すと削除します", "開く/変更", "新しい値", "元に戻す", "削除", "上へ",
        "エントリー", "場所：%s", "名前", "種類", "値", "数値", "文字列", "10進数", "16進数", "入力", "消す",
        "10進/16進", "入力欄", "スペース", "削除", "値の名前を入力してください。", "数値が大きすぎます。",
    },
    {
        "게임 레지스트리",
        "게임이 해상도, 언어, 조작 같은 설정을 저장하는 Windows 레지스트리 항목입니다. 먼저 앱에서 준비한 "
        "항목이, 그다음 이 게임의 레지스트리 전체가 나옵니다.",
        "앱에서 · %d", "레지스트리 전체", "컴퓨터",
        { "시스템 설정: 게임 설치 위치, 버전, 모든 사용자가 함께 쓰는 설정.",
            "사용자 설정. 게임은 보통 여기에 해상도, 언어, 조작 설정을 저장합니다." },
        "키: 하위 키 %d개 · 값 %d개", "이 키는 비어 있습니다.", "아직 없는 키입니다. 게임이 아직 만들지 않았습니다.",
        { "숫자 (REG_DWORD).", "숫자 (REG_QWORD).", "문자열 (REG_SZ).", "변수 포함 문자열 (REG_EXPAND_SZ).",
            "문자열 목록 (REG_MULTI_SZ).", "바이너리 데이터 (REG_BINARY)." },
        "종류 %u의 데이터.", { "신규", "삭제됨", "변경됨" }, "이전: %s · Y — 되돌리기", "경로:", "레지스트리에 쓰는 값:",
        "직접 입력한 값", "게임을 다시 시작하면 적용됩니다. 앱에서 준비한 항목입니다.",
        "A — 변경. 게임을 다시 시작하면 새 값을 읽습니다.",
        "새 값입니다. 게임을 다시 시작하면 레지스트리에 나타납니다.",
        "게임을 다시 시작하면 삭제됩니다. A 또는 Y — 유지.", "이런 데이터는 앱에서 편집합니다.", "A — 열기",
        "X — 값 만들기", "삭제하려면 −를 한 번 더 누르세요", "열기 / 변경", "새 값", "되돌리기", "삭제", "위로",
        "항목", "%s 안에", "이름", "종류", "값", "숫자", "문자열", "10진수", "16진수", "누르기", "지우기",
        "10진 / 16진", "입력란", "Space", "삭제", "값 이름을 입력하세요.", "숫자가 너무 큽니다.",
    },
    {
        "遊戲登錄檔",
        "遊戲用來保存設定的 Windows 登錄檔項目：解析度、語言、操作。先列出在應用程式中準備的項目，"
        "再列出這款遊戲的整個登錄檔。",
        "來自應用程式 · %d", "整個登錄檔", "電腦",
        { "系統設定：遊戲的安裝位置、版本、所有人共用的設定。",
            "使用者設定。遊戲通常把解析度、語言和操作存在這裡。" },
        "機碼：%d 個子機碼 · %d 個值", "這個機碼是空的。", "這個機碼還不存在：遊戲尚未建立它。",
        { "數字（REG_DWORD）。", "數字（REG_QWORD）。", "字串（REG_SZ）。", "含變數的字串（REG_EXPAND_SZ）。",
            "字串清單（REG_MULTI_SZ）。", "二進位資料（REG_BINARY）。" },
        "類型 %u 的資料。", { "新增", "已刪除", "已變更" }, "原本：%s · Y — 復原", "路徑：", "寫入登錄檔：",
        "自訂值", "重新啟動遊戲後生效。此項目是在應用程式中準備的。",
        "A — 變更。遊戲重新啟動後會讀取新值。",
        "新值。遊戲重新啟動後會出現在登錄檔中。",
        "遊戲重新啟動時刪除。A 或 Y — 保留。", "這類資料請在應用程式中編輯。", "A — 開啟",
        "X — 建立值", "再按一次 − 以刪除", "開啟 / 變更", "新值", "復原", "刪除", "上一層",
        "項目", "位於 %s", "名稱", "類型", "值", "數字", "字串", "十進位", "十六進位", "按下", "退格",
        "十 / 十六進位", "欄位", "空格", "刪除", "請輸入值的名稱。", "數字太大。",
    },
    {
        "Registre du jeu",
        "Entrées du registre Windows où le jeu garde ses paramètres : résolution, langue, commandes. D’abord les "
        "entrées préparées dans l’application, puis tout le registre du jeu.",
        "De l’application · %d", "Tout le registre", "Ordinateur",
        { "Paramètres du système : emplacement du jeu, sa version, paramètres communs à tous.",
            "Paramètres de l’utilisateur. Les jeux y gardent en général leur résolution, leur langue et leurs "
            "commandes." },
        "Clé : %d sous-clés · %d valeurs", "Cette clé est vide.",
        "Cette clé n’existe pas encore : le jeu ne l’a pas créée.",
        { "Nombre (REG_DWORD).", "Nombre (REG_QWORD).", "Chaîne (REG_SZ).", "Chaîne avec variables (REG_EXPAND_SZ).",
            "Liste de chaînes (REG_MULTI_SZ).", "Données binaires (REG_BINARY)." },
        "Données de type %u.", { "nouvelle", "supprimée", "modifiée" }, "Avant : %s · Y — annuler", "Chemin :",
        "Écrit dans le registre :", "Valeur perso",
        "Prend effet au redémarrage du jeu. L’entrée a été préparée dans l’application.",
        "A — modifier. Le jeu lira la nouvelle valeur au redémarrage.",
        "Nouvelle valeur. Elle apparaîtra dans le registre au redémarrage du jeu.",
        "Supprimée au redémarrage du jeu. A ou Y — la garder.", "Ce type de données se modifie dans l’application.",
        "A — ouvrir", "X — créer une valeur", "Appuyez encore sur − pour supprimer", "Ouvrir / modifier",
        "Nouvelle valeur", "Annuler", "Supprimer", "Remonter", "Entrées", "Dans %s", "Nom", "Type", "Valeur", "Nombre",
        "Chaîne", "Décimal", "Hexadécimal", "Appuyer", "Effacer", "Déc. / hex.", "Champ", "Espace", "suppression",
        "Saisissez un nom pour la valeur.", "Le nombre est trop grand.",
    },
    {
        "Registre du jeu",
        "Entrées du registre Windows où le jeu garde ses paramètres : résolution, langue, commandes. D’abord les "
        "entrées préparées dans l’application, puis le registre complet du jeu.",
        "De l’application · %d", "Registre complet", "Ordinateur",
        { "Paramètres du système : emplacement d’installation du jeu, sa version, paramètres communs à tous.",
            "Paramètres de l’utilisateur. Les jeux y gardent généralement leur résolution, leur langue et leurs "
            "commandes." },
        "Clé : %d sous-clés · %d valeurs", "Cette clé est vide.",
        "Cette clé n’existe pas encore : le jeu ne l’a pas créée.",
        { "Nombre (REG_DWORD).", "Nombre (REG_QWORD).", "Chaîne (REG_SZ).", "Chaîne avec variables (REG_EXPAND_SZ).",
            "Liste de chaînes (REG_MULTI_SZ).", "Données binaires (REG_BINARY)." },
        "Données de type %u.", { "nouveau", "supprimé", "modifié" }, "Avant : %s · Y — défaire", "Chemin :",
        "Écrit dans le registre :", "Valeur personnalisée",
        "S’applique au redémarrage du jeu. L’entrée a été préparée dans l’application.",
        "A — modifier. Le jeu lira la nouvelle valeur au redémarrage.",
        "Nouvelle valeur. Elle apparaîtra dans le registre au redémarrage du jeu.",
        "Supprimée au redémarrage du jeu. A ou Y — la garder.", "Ce type de données se modifie dans l’application.",
        "A — ouvrir", "X — créer une valeur", "Appuyez de nouveau sur − pour supprimer", "Ouvrir / modifier",
        "Nouvelle valeur", "Défaire", "Supprimer", "Remonter", "Entrées", "Dans %s", "Nom", "Type", "Valeur",
        "Nombre", "Chaîne", "Décimal", "Hexadécimal", "Appuyer", "Effacer", "Déc. / hex.", "Champ", "Espace",
        "supprimer", "Entrez un nom pour la valeur.", "Le nombre est trop grand.",
    },
    {
        "Spiel-Registry",
        "Einträge der Windows-Registry, in denen das Spiel seine Einstellungen speichert: Auflösung, Sprache, "
        "Steuerung. Zuerst die in der App vorbereiteten Einträge, dann die gesamte Registry des Spiels.",
        "Aus der App · %d", "Ganze Registry", "Computer",
        { "Einstellungen für den ganzen Computer: wo das Spiel installiert ist, seine Version, gemeinsame "
            "Einstellungen für alle.",
            "Benutzereinstellungen. Spiele speichern hier meist Auflösung, Sprache und Steuerung." },
        "Schlüssel: %d Unterschl. · %d Werte", "Dieser Schlüssel ist leer.",
        "Diesen Schlüssel gibt es noch nicht: Das Spiel hat ihn nicht angelegt.",
        { "Zahl (REG_DWORD).", "Zahl (REG_QWORD).", "Text (REG_SZ).", "Text mit Variablen (REG_EXPAND_SZ).",
            "Textliste (REG_MULTI_SZ).", "Binärdaten (REG_BINARY)." },
        "Daten vom Typ %u.", { "neu", "gelöscht", "geändert" }, "Vorher: %s · Y — rückgängig", "Pfad:",
        "Schreibt in die Registry:", "Eigener Wert",
        "Wirkt nach einem Neustart des Spiels. Der Eintrag wurde in der App vorbereitet.",
        "A — ändern. Das Spiel liest den neuen Wert nach einem Neustart.",
        "Neuer Wert. Er erscheint nach einem Neustart des Spiels in der Registry.",
        "Wird beim Neustart des Spiels gelöscht. A oder Y — behalten.",
        "Daten dieser Art werden in der App bearbeitet.", "A — öffnen", "X — Wert anlegen",
        "Drücke erneut −, um zu löschen", "Öffnen / ändern", "Neuer Wert", "Rückgängig", "Löschen", "Nach oben",
        "Einträge", "In %s", "Name", "Typ", "Wert", "Zahl", "Text", "Dezimal", "Hexadezimal", "Drücken", "Löschen",
        "Dez / hex", "Feld", "Leertaste", "löschen", "Gib einen Namen für den Wert ein.", "Die Zahl ist zu groß.",
    },
    {
        "Registro del gioco",
        "Voci del registro di Windows in cui il gioco conserva le sue impostazioni: risoluzione, lingua, comandi. "
        "Prima le voci preparate nell'app, poi l'intero registro del gioco.",
        "Dall'app · %d", "Intero registro", "Computer",
        { "Impostazioni di sistema: dove è installato il gioco, la sua versione, impostazioni comuni a tutti.",
            "Impostazioni dell'utente. Di solito i giochi conservano qui risoluzione, lingua e comandi." },
        "Chiave: %d sottochiavi · %d valori", "Questa chiave è vuota.",
        "Questa chiave non esiste ancora: il gioco non l'ha creata.",
        { "Numero (REG_DWORD).", "Numero (REG_QWORD).", "Stringa (REG_SZ).", "Stringa con variabili (REG_EXPAND_SZ).",
            "Elenco di stringhe (REG_MULTI_SZ).", "Dati binari (REG_BINARY)." },
        "Dati di tipo %u.", { "nuovo", "eliminato", "modificato" }, "Prima: %s · Y — annulla", "Percorso:",
        "Scrive nel registro:", "Valore personalizzato",
        "Ha effetto al riavvio del gioco. La voce è stata preparata nell'app.",
        "A — cambia. Il gioco legge il nuovo valore al riavvio.",
        "Nuovo valore. Compare nel registro al riavvio del gioco.",
        "Verrà eliminato al riavvio del gioco. A o Y — mantieni.", "Questo tipo di dati si modifica nell'app.",
        "A — apri", "X — crea un valore", "Premi di nuovo − per eliminare", "Apri / cambia", "Nuovo valore", "Annulla",
        "Elimina", "Su", "Voci", "In %s", "Nome", "Tipo", "Valore", "Numero", "Stringa", "Decimale", "Esadecimale",
        "Premi", "Cancella", "Dec / esa", "Campo", "Spazio", "elimina", "Inserisci un nome per il valore.",
        "Il numero è troppo grande.",
    },
    {
        "Registro del juego",
        "Entradas del Registro de Windows donde el juego guarda sus ajustes: resolución, idioma, controles. Primero, "
        "las entradas preparadas en la aplicación; después, todo el registro del juego.",
        "De la aplicación · %d", "Todo el registro", "Equipo",
        { "Ajustes del sistema: dónde está instalado el juego, su versión, ajustes comunes para todos.",
            "Ajustes del usuario. Aquí suelen guardar los juegos la resolución, el idioma y los controles." },
        "Clave: %d subclaves · %d valores", "Esta clave está vacía.",
        "Esta clave aún no existe: el juego no la ha creado.",
        { "Número (REG_DWORD).", "Número (REG_QWORD).", "Cadena (REG_SZ).", "Cadena con variables (REG_EXPAND_SZ).",
            "Lista de cadenas (REG_MULTI_SZ).", "Datos binarios (REG_BINARY)." },
        "Datos de tipo %u.", { "nuevo", "eliminado", "cambiado" }, "Antes: %s · Y — deshacer", "Ruta:",
        "Escribe en el registro:", "Valor propio",
        "Se aplicará cuando reinicies el juego. La entrada se preparó en la aplicación.",
        "A — cambiar. El juego leerá el nuevo valor cuando se reinicie.",
        "Valor nuevo. Aparecerá en el registro cuando reinicies el juego.",
        "Se eliminará cuando reinicies el juego. A o Y — conservarlo.", "Este tipo de datos se edita en la aplicación.",
        "A — abrir", "X — crear un valor", "Pulsa − otra vez para eliminar", "Abrir / cambiar", "Valor nuevo",
        "Deshacer", "Eliminar", "Subir", "Entradas", "En %s", "Nombre", "Tipo", "Valor", "Número", "Cadena",
        "Decimal", "Hexadecimal", "Pulsar", "Borrar", "Dec / hex", "Campo", "Espacio", "eliminado",
        "Escribe un nombre para el valor.", "El número es demasiado grande.",
    },
    {
        "Registro del juego",
        "Entradas del registro de Windows donde el juego guarda su configuración: resolución, idioma, controles. "
        "Primero, las entradas preparadas en la aplicación; después, todo el registro del juego.",
        "De la aplicación · %d", "Todo el registro", "Equipo",
        { "Configuración del sistema: dónde está instalado el juego, su versión y los ajustes comunes para todos.",
            "Configuración del usuario. Aquí los juegos suelen guardar su resolución, idioma y controles." },
        "Clave: %d subclaves · %d valores", "Esta clave está vacía.", "Esta clave aún no existe: el juego no la creó.",
        { "Número (REG_DWORD).", "Número (REG_QWORD).", "Cadena (REG_SZ).", "Cadena con variables (REG_EXPAND_SZ).",
            "Lista de cadenas (REG_MULTI_SZ).", "Datos binarios (REG_BINARY)." },
        "Datos de tipo %u.", { "nuevo", "eliminado", "cambiado" }, "Antes: %s · Y — deshacer", "Ruta:",
        "Escribe en el registro:", "Valor propio",
        "Se aplica al reiniciar el juego. La entrada se preparó en la aplicación.",
        "A — cambiar. El juego leerá el nuevo valor al reiniciarse.",
        "Valor nuevo. Aparecerá en el registro al reiniciar el juego.",
        "Se eliminará al reiniciar el juego. A o Y — conservarlo.", "Este tipo de datos se edita en la aplicación.",
        "A — abrir", "X — crear un valor", "Presiona − otra vez para eliminar", "Abrir / cambiar", "Valor nuevo",
        "Deshacer", "Eliminar", "Subir", "Entradas", "En %s", "Nombre", "Tipo", "Valor", "Número", "Cadena", "Decimal",
        "Hexadecimal", "Presionar", "Borrar", "Dec / hex", "Campo", "Espacio", "eliminar",
        "Escribe un nombre para el valor.", "El número es demasiado grande.",
    },
    {
        "Gameregister",
        "Vermeldingen in het Windows-register waarin de game zijn instellingen bewaart: resolutie, taal, besturing. "
        "Eerst de vermeldingen die in de app zijn voorbereid, daarna het hele register van de game.",
        "Uit de app · %d", "Hele register", "Computer",
        { "Systeeminstellingen: waar de game is geïnstalleerd, de versie, instellingen die voor iedereen gelden.",
            "Gebruikersinstellingen. Games bewaren hier meestal hun resolutie, taal en besturing." },
        "Sleutel: %d subsleutels · %d waarden", "Deze sleutel is leeg.",
        "Deze sleutel bestaat nog niet: de game heeft hem nog niet gemaakt.",
        { "Getal (REG_DWORD).", "Getal (REG_QWORD).", "Tekenreeks (REG_SZ).",
            "Tekenreeks met variabelen (REG_EXPAND_SZ).", "Lijst met tekenreeksen (REG_MULTI_SZ).",
            "Binaire gegevens (REG_BINARY)." },
        "Gegevens van type %u.", { "nieuw", "verwijderd", "gewijzigd" }, "Was: %s · Y — ongedaan maken", "Pad:",
        "Schrijft naar het register:", "Eigen waarde",
        "Werkt na een herstart van de game. De vermelding is in de app voorbereid.",
        "A — wijzigen. De game leest de nieuwe waarde na een herstart.",
        "Nieuwe waarde. Verschijnt na een herstart van de game in het register.",
        "Wordt verwijderd bij een herstart van de game. A of Y — behouden.", "Dit soort gegevens bewerk je in de app.",
        "A — openen", "X — waarde maken", "Druk nogmaals op − om te verwijderen", "Openen / wijzigen", "Nieuwe waarde",
        "Ongedaan maken", "Verwijderen", "Omhoog", "Vermeldingen", "In %s", "Naam", "Type", "Waarde", "Getal",
        "Tekenreeks", "Decimaal", "Hexadecimaal", "Indrukken", "Wissen", "Dec / hex", "Veld", "Spatie", "verwijderen",
        "Voer een naam voor de waarde in.", "Het getal is te groot.",
    },
    {
        "Registo do jogo",
        "Entradas do registo do Windows onde o jogo guarda as suas definições: resolução, idioma, controlos. "
        "Primeiro as entradas preparadas na aplicação, depois todo o registo do jogo.",
        "Da aplicação · %d", "Registo completo", "Computador",
        { "Definições do sistema: onde o jogo está instalado, a versão, definições partilhadas por todos.",
            "Definições do utilizador. Os jogos costumam guardar aqui a resolução, o idioma e os controlos." },
        "Chave: %d subchaves · %d valores", "Esta chave está vazia.",
        "Esta chave ainda não existe: o jogo ainda não a criou.",
        { "Número (REG_DWORD).", "Número (REG_QWORD).", "Texto (REG_SZ).", "Texto com variáveis (REG_EXPAND_SZ).",
            "Lista de textos (REG_MULTI_SZ).", "Dados binários (REG_BINARY)." },
        "Dados do tipo %u.", { "novo", "eliminado", "alterado" }, "Antes: %s · Y — anular", "Caminho:",
        "Escreve no registo:", "Valor próprio",
        "Tem efeito quando o jogo reiniciar. A entrada foi preparada na aplicação.",
        "A — alterar. O jogo lê o novo valor quando reiniciar.",
        "Valor novo. Aparece no registo quando o jogo reiniciar.",
        "Eliminado quando o jogo reiniciar. A ou Y — manter.", "Dados deste tipo são editados na aplicação.",
        "A — abrir", "X — criar um valor", "Prima − novamente para eliminar", "Abrir / alterar", "Valor novo",
        "Anular", "Eliminar", "Subir", "Entradas", "Em %s", "Nome", "Tipo", "Valor", "Número", "Texto", "Decimal",
        "Hexadecimal", "Premir", "Apagar", "Dec / hex", "Campo", "Espaço", "eliminar",
        "Introduza um nome para o valor.", "O número é demasiado grande.",
    },
    {
        "Registro do jogo",
        "Entradas do registro do Windows em que o jogo guarda suas configurações: resolução, idioma, controles. "
        "Primeiro as entradas preparadas no app, depois todo o registro do jogo.",
        "Do app · %d", "Registro inteiro", "Computador",
        { "Configurações do sistema: onde o jogo está instalado, a versão, configurações comuns a todos.",
            "Configurações do usuário. Os jogos costumam guardar aqui a resolução, o idioma e os controles." },
        "Chave: %d subchaves · %d valores", "Esta chave está vazia.",
        "Esta chave ainda não existe: o jogo não a criou.",
        { "Número (REG_DWORD).", "Número (REG_QWORD).", "Texto (REG_SZ).", "Texto com variáveis (REG_EXPAND_SZ).",
            "Lista de textos (REG_MULTI_SZ).", "Dados binários (REG_BINARY)." },
        "Dados do tipo %u.", { "novo", "excluído", "alterado" }, "Antes: %s · Y — desfazer", "Caminho:",
        "Grava no registro:", "Valor personalizado",
        "Tem efeito quando o jogo reiniciar. A entrada foi preparada no app.",
        "A — alterar. O jogo lê o novo valor quando reiniciar.",
        "Valor novo. Aparece no registro quando o jogo reiniciar.",
        "Será excluído quando o jogo reiniciar. A ou Y — manter.", "Dados deste tipo são editados no app.",
        "A — abrir", "X — criar um valor", "Pressione − de novo para excluir", "Abrir / alterar", "Novo valor",
        "Desfazer", "Excluir", "Subir", "Entradas", "Em %s", "Nome", "Tipo", "Valor", "Número", "Texto", "Decimal",
        "Hexadecimal", "Pressionar", "Apagar", "Dec / hex", "Campo", "Espaço", "excluir",
        "Digite um nome para o valor.", "O número é grande demais.",
    },
};

_Static_assert(sizeof(REG_STRINGS) / sizeof(REG_STRINGS[0]) == CarafeLanguageCount, "registry texts per language");

typedef struct {
    int source;
    int column;
    int row;
    const char* mark;
} Cell;

typedef struct {
    int label;
    bool cross;
    Cell cells[5];
    int count;
} MapRow;

enum {
    S_A, S_B, S_X, S_Y, S_L, S_R, S_ZL, S_ZR, S_PLUS, S_MINUS, S_STICKL, S_STICKR,
    S_UP, S_DOWN, S_LEFT, S_RIGHT, S_LUP, S_LDOWN, S_LLEFT, S_LRIGHT, S_RUP, S_RDOWN, S_RLEFT, S_RRIGHT,
};

static const MapRow MAP[2][5] = {
    {
        { 0, false, { { S_ZL, 0, 0, "" } }, 1 },
        { 1, false, { { S_L, 0, 0, "" } }, 1 },
        { 2, false, { { S_MINUS, 0, 0, "" } }, 1 },
        { 3, true, { { S_LUP, 1, 0, "↑" }, { S_LLEFT, 0, 1, "←" }, { S_STICKL, 1, 1, "" }, { S_LRIGHT, 2, 1, "→" },
              { S_LDOWN, 1, 2, "↓" } }, 5 },
        { 4, true, { { S_UP, 1, 0, "↑" }, { S_LEFT, 0, 1, "←" }, { S_RIGHT, 2, 1, "→" }, { S_DOWN, 1, 2, "↓" } }, 4 },
    },
    {
        { 5, false, { { S_ZR, 0, 0, "" } }, 1 },
        { 6, false, { { S_R, 0, 0, "" } }, 1 },
        { 7, false, { { S_PLUS, 0, 0, "" } }, 1 },
        { 8, true, { { S_X, 1, 0, "X" }, { S_Y, 0, 1, "Y" }, { S_A, 2, 1, "A" }, { S_B, 1, 2, "B" } }, 4 },
        { 9, true, { { S_RUP, 1, 0, "↑" }, { S_RLEFT, 0, 1, "←" }, { S_STICKR, 1, 1, "" }, { S_RRIGHT, 2, 1, "→" },
              { S_RDOWN, 1, 2, "↓" } }, 5 },
    },
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_picture_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_open;
static bool g_error_mode;
static CarafeFailure g_failure = CarafeKindNone;
static CarafeCatalog* g_catalog;
static CarafeLanguage g_language = CarafeLanguageEn;
static unsigned int g_generation = 1;
static unsigned int g_rendered;
static CarafeImage g_picture;
static CarafeImage g_icon;
static char g_title[128];
static int g_tab;
static int g_focus;
static int g_scroll;
static int g_group = -1;
static int g_page;
static int g_group_focus;
static int g_group_scroll;
static int g_pad_focus = STRIP_MAX;
static bool g_picker;
static int g_picker_source;
static bool g_picker_pad;
static CarafeChord g_picker_chord;
static int g_picker_focus;
static bool g_confirm;
static int g_confirm_focus;
static bool g_trial = true;
static bool g_reset_armed;
static unsigned int g_held;
static unsigned int g_repeat_bit;
static uint64_t g_repeat_at;
static bool g_touching;
static u64 g_chord_held;
static Hit g_hits[MAX_HITS];
static int g_hit_count;
static bool g_reg_available;
static bool g_reg;
static int g_reg_view;
static int g_reg_hive = -1;
static char g_reg_path[CARAFE_REG_PATH];
static int g_reg_focus;
static int g_reg_scroll;
static int g_reg_row_focus;
static int g_reg_row_scroll;
static bool g_reg_armed;
static CarafeRegEntry* g_reg_entries;
static int g_reg_count;
static bool g_reg_missing;
static bool g_edit;
static bool g_edit_new;
static int g_edit_field;
static char g_edit_name[CARAFE_REG_NAME];
static char g_edit_text[CARAFE_REG_NAME];
static uint32_t g_edit_type;
static bool g_edit_hex;
static bool g_edit_shift;
static int g_edit_key;
static const char* g_edit_error;

static void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    carafeOverlayLog(line);
}

static const Strings* text(void) {
    return &STRINGS[g_language];
}

static const char* pick(const char* const* texts) {
    return carafeText(texts);
}

static const RegStrings* reg(void) {
    return &REG_STRINGS[g_language];
}

static int pendingTotal(void) {
    return carafePendingCount(g_catalog) + (g_reg_available ? carafeRegistryPendingCount() : 0);
}

static void changed(void) {
    g_generation++;
    if (g_generation == 0) {
        g_generation = 1;
    }
}

static int tabCount(void) {
    return g_catalog->section_count + (g_error_mode ? 1 : 0);
}

static const CarafeSection* tabSection(int tab) {
    int index = g_error_mode ? tab - 1 : tab;
    return index >= 0 && index < g_catalog->section_count ? &g_catalog->sections[index] : NULL;
}

static bool tabIsTry(int tab) {
    return g_error_mode && tab == 0;
}

static bool tabIsControls(int tab) {
    const CarafeSection* section = tabSection(tab);
    return section != NULL && strcmp(section->id, "controls") == 0;
}

static bool settingActive(const CarafeSetting* setting) {
    return carafeWhenHolds(g_catalog, &setting->when, CarafeWhenOn);
}

static bool groupVisible(const CarafeGroup* group) {
    return carafeWhenHolds(g_catalog, &group->when, CarafeWhenShow);
}

static int buildItems(Item* items) {
    int count = 0;
    if (g_group >= 0) {
        const CarafeGroup* group = &g_catalog->groups[g_group];
        if (g_page >= group->page_count) {
            return 0;
        }
        const CarafePage* page = &group->pages[g_page];
        for (int i = 0; i < page->row_count && count < MAX_ITEMS; i++) {
            const CarafeSetting* setting = &g_catalog->settings[page->rows[i].index];
            if (page->rows[i].kind == CarafeRowSetting && carafeSettingVisible(g_catalog, setting)) {
                items[count++] = (Item){ ItemSetting, page->rows[i].index, 0 };
            }
        }
        return count;
    }
    if (tabIsTry(g_tab)) {
        for (int i = 0; i < g_catalog->suggest_count && count < MAX_ITEMS; i++) {
            const CarafeSuggest* suggest = &g_catalog->suggests[i];
            if (suggest->kind == g_failure && carafeSettingVisible(g_catalog, &g_catalog->settings[suggest->setting])) {
                items[count++] = (Item){ ItemSuggest, i, suggest->value };
            }
        }
        return count;
    }
    const CarafeSection* section = tabSection(g_tab);
    if (section == NULL) {
        return 0;
    }
    if (section->preset_count > 0) {
        items[count++] = (Item){ ItemPreset, 0, 0 };
    }
    for (int i = 0; i < section->row_count && count < MAX_ITEMS; i++) {
        const CarafeRow* row = &section->rows[i];
        if (row->kind == CarafeRowSetting) {
            if (carafeSettingVisible(g_catalog, &g_catalog->settings[row->index])) {
                items[count++] = (Item){ ItemSetting, row->index, 0 };
            }
        } else if (groupVisible(&g_catalog->groups[row->index])) {
            items[count++] = (Item){ ItemGroup, row->index, 0 };
        }
    }
    if (strcmp(section->id, "system") == 0 && g_reg_available && !g_error_mode && count < MAX_ITEMS) {
        items[count++] = (Item){ ItemRegistry, 0, 0 };
    }
    if (strcmp(section->id, "system") == 0 && count < MAX_ITEMS) {
        items[count++] = (Item){ ItemReset, 0, 0 };
    }
    return count;
}

static int* focusRef(void) {
    return g_group >= 0 ? &g_group_focus : &g_focus;
}

static int* scrollRef(void) {
    return g_group >= 0 ? &g_group_scroll : &g_scroll;
}

static int defaultIndex(const CarafeSetting* setting) {
    if (setting->base >= 0 && setting->base < setting->value_count) {
        return setting->base;
    }
    for (int i = 0; i < setting->value_count; i++) {
        if (strcmp(setting->values[i].text, "-") == 0) {
            return i;
        }
    }
    return setting->default_index >= 0 && setting->default_index < setting->value_count ? setting->default_index : 0;
}

static void stepSetting(int index, int delta) {
    CarafeSetting* setting = &g_catalog->settings[index];
    if (!settingActive(setting) || setting->value_count == 0) {
        return;
    }
    int current = setting->current < 0 ? defaultIndex(setting) : setting->current;
    int next = (current + delta + setting->value_count) % setting->value_count;
    carafeSettingSet(g_catalog, index, next);
}

static void stepPreset(int delta) {
    const CarafeSection* section = tabSection(g_tab);
    if (section == NULL || section->preset_count == 0) {
        return;
    }
    int current = carafePresetCurrent(g_catalog, section);
    int next = current < 0 ? (delta > 0 ? 0 : section->preset_count - 1)
                           : (current + delta + section->preset_count) % section->preset_count;
    carafePresetApply(g_catalog, &section->presets[next]);
}

static bool regHasRows(void) {
    return carafeRegistryRowCount() > 0;
}

static void regRefresh(void) {
    g_reg_missing = false;
    g_reg_count = 0;
    if (g_reg_view != 1 || g_reg_hive < 0 || g_reg_entries == NULL) {
        return;
    }
    int count = carafeRegistryList(g_reg_hive, g_reg_path, g_reg_entries, REG_MAX_ENTRIES);
    if (count < 0) {
        g_reg_missing = true;
        count = 0;
    }
    g_reg_count = count;
}

static int regListCount(void) {
    if (g_reg_view == 0) {
        return carafeRegistryRowCount();
    }
    return g_reg_hive < 0 ? 2 : g_reg_count;
}

static int* regFocus(void) {
    return g_reg_view == 0 ? &g_reg_row_focus : &g_reg_focus;
}

static int* regScroll(void) {
    return g_reg_view == 0 ? &g_reg_row_scroll : &g_reg_scroll;
}

static int rootHive(int index) {
    return index == 0 ? CARAFE_REG_HIVE_USER : CARAFE_REG_HIVE_MACHINE;
}

static void regShowView(int view) {
    g_reg_view = view;
    g_reg_armed = false;
    regRefresh();
}

static void regEnter(void) {
    if (g_reg_entries == NULL) {
        g_reg_entries = calloc(REG_MAX_ENTRIES, sizeof(CarafeRegEntry));
    }
    g_reg = true;
    g_reg_hive = -1;
    g_reg_path[0] = '\0';
    g_reg_focus = g_reg_scroll = 0;
    g_reg_row_focus = g_reg_row_scroll = 0;
    g_edit = false;
    regShowView(regHasRows() ? 0 : 1);
}

static void regLeave(void) {
    g_reg = false;
    g_edit = false;
    g_reg_armed = false;
}

static void regDescend(const char* name, int root) {
    if (g_reg_hive < 0) {
        g_reg_hive = rootHive(root);
        g_reg_path[0] = '\0';
    } else {
        size_t used = strlen(g_reg_path);
        if (used + strlen(name) + 2 >= sizeof(g_reg_path)) {
            return;
        }
        snprintf(g_reg_path + used, sizeof(g_reg_path) - used, "%s%s", used > 0 ? "\\" : "", name);
    }
    g_reg_focus = g_reg_scroll = 0;
    g_reg_armed = false;
    regRefresh();
}

static void regAscend(void) {
    char* slash = strrchr(g_reg_path, '\\');
    char leaf[CARAFE_REG_PATH] = "";
    int root = g_reg_hive == CARAFE_REG_HIVE_USER ? 0 : 1;
    if (g_reg_path[0] == '\0') {
        g_reg_hive = -1;
    } else if (slash != NULL) {
        snprintf(leaf, sizeof(leaf), "%s", slash + 1);
        *slash = '\0';
    } else {
        snprintf(leaf, sizeof(leaf), "%s", g_reg_path);
        g_reg_path[0] = '\0';
    }
    g_reg_scroll = 0;
    g_reg_focus = 0;
    g_reg_armed = false;
    regRefresh();
    if (g_reg_hive < 0) {
        g_reg_focus = root;
        return;
    }
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg_entries[i].key && strcasecmp(g_reg_entries[i].name, leaf) == 0) {
            g_reg_focus = i;
        }
    }
}

static bool regEditable(const CarafeRegEntry* entry) {
    if (entry->key || entry->truncated) {
        return false;
    }
    return (entry->type == CARAFE_REG_DWORD && entry->size == 4) || (entry->type == CARAFE_REG_QWORD && entry->size == 8)
        || entry->type == CARAFE_REG_SZ || entry->type == CARAFE_REG_EXPAND_SZ;
}

static bool regNumber(uint32_t type) {
    return type == CARAFE_REG_DWORD || type == CARAFE_REG_QWORD;
}

static void regOpenEditor(const CarafeRegEntry* entry) {
    g_edit = true;
    g_edit_new = entry == NULL;
    g_edit_field = 2;
    g_edit_hex = false;
    g_edit_shift = false;
    g_edit_key = 0;
    g_edit_error = NULL;
    if (entry == NULL) {
        g_edit_name[0] = '\0';
        g_edit_text[0] = '\0';
        g_edit_type = CARAFE_REG_DWORD;
        g_edit_field = 0;
        return;
    }
    snprintf(g_edit_name, sizeof(g_edit_name), "%s", entry->name);
    g_edit_type = entry->type;
    carafeRegistryFormat(entry->type, entry->data, entry->size, g_edit_text, sizeof(g_edit_text));
}

static bool regEditCommit(void) {
    const RegStrings* r = reg();
    g_edit_error = NULL;
    if (g_edit_new && g_edit_name[0] == '\0') {
        g_edit_error = r->name_needed;
        return false;
    }
    uint8_t data[CARAFE_REG_DATA];
    uint32_t size = 0;
    if (regNumber(g_edit_type)) {
        errno = 0;
        unsigned long long value = g_edit_text[0] != '\0' ? strtoull(g_edit_text, NULL, g_edit_hex ? 16 : 10) : 0;
        if (errno != 0 || (g_edit_type == CARAFE_REG_DWORD && value > 0xFFFFFFFFULL)) {
            g_edit_error = r->too_big;
            return false;
        }
        size = g_edit_type == CARAFE_REG_DWORD ? 4 : 8;
        for (uint32_t i = 0; i < size; i++) {
            data[i] = (uint8_t)(value >> (8 * i));
        }
    } else {
        uint16_t wide[CARAFE_REG_DATA / 2];
        size_t count = carafeRegistryUtf16(g_edit_text, wide, CARAFE_REG_DATA / 2 - 1);
        wide[count++] = 0;
        for (size_t i = 0; i < count; i++) {
            data[2 * i] = (uint8_t)(wide[i] & 0xFF);
            data[2 * i + 1] = (uint8_t)(wide[i] >> 8);
        }
        size = (uint32_t)count * 2;
    }
    carafeRegistrySetPending(g_reg_hive, g_reg_path, g_edit_name, g_edit_type, data, size);
    g_edit = false;
    regRefresh();
    for (int i = 0; i < g_reg_count; i++) {
        if (!g_reg_entries[i].key && strcasecmp(g_reg_entries[i].name, g_edit_name) == 0) {
            g_reg_focus = i;
        }
    }
    note("[CARAFE] settings menu: registry %s\\%s › %s changed", carafeRegistryHiveName(g_reg_hive), g_reg_path,
        g_edit_name);
    return true;
}

static void regRowStep(int row, int delta) {
    int count = carafeRegistryRowOptionCount(row);
    if (count == 0) {
        return;
    }
    int current = carafeRegistryRowCurrent(row);
    int next = current < 0 ? (delta > 0 ? 0 : count - 1) : (current + delta + count) % count;
    carafeRegistryRowSet(row, next);
}

static void regActivate(int index, int delta) {
    if (g_reg_view == 0) {
        regRowStep(index, delta);
        return;
    }
    if (delta <= 0) {
        return;
    }
    if (g_reg_hive < 0) {
        regDescend("", index);
        return;
    }
    if (index < 0 || index >= g_reg_count) {
        return;
    }
    CarafeRegEntry* entry = &g_reg_entries[index];
    if (entry->key) {
        if (entry->state != CarafeRegDeleted) {
            regDescend(entry->name, 0);
        }
        return;
    }
    if (entry->state == CarafeRegDeleted) {
        carafeRegistryDropPending(g_reg_hive, g_reg_path, entry->name);
        regRefresh();
        return;
    }
    if (regEditable(entry)) {
        regOpenEditor(entry);
    }
}

static void activateItem(const Item* item, int delta) {
    switch (item->kind) {
    case ItemRegistry:
        if (delta > 0) {
            regEnter();
        }
        break;
    case ItemSetting:
        stepSetting(item->index, delta);
        break;
    case ItemGroup:
        if (delta > 0) {
            g_group = item->index;
            g_page = 0;
            g_group_focus = 0;
            g_group_scroll = 0;
        }
        break;
    case ItemPreset:
        stepPreset(delta);
        break;
    case ItemReset:
        if (delta > 0) {
            if (g_reset_armed) {
                carafeResetAll(g_catalog, !g_error_mode);
                g_reset_armed = false;
                note("[CARAFE] settings menu: recommended settings restored");
            } else {
                g_reset_armed = true;
            }
        }
        break;
    case ItemSuggest: {
        const CarafeSuggest* suggest = &g_catalog->suggests[item->index];
        CarafeSetting* setting = &g_catalog->settings[suggest->setting];
        int target = setting->current == suggest->value ? (setting->start != suggest->value && setting->start >= 0
                                                                   ? setting->start
                                                                   : defaultIndex(setting))
                                                        : suggest->value;
        carafeSettingSet(g_catalog, suggest->setting, target);
        break;
    }
    }
}

static void resetItem(const Item* item) {
    if (item->kind == ItemSetting) {
        CarafeSetting* setting = &g_catalog->settings[item->index];
        if (settingActive(setting)) {
            carafeSettingSet(g_catalog, item->index, defaultIndex(setting));
        }
    }
}

static bool keyboardMode(void) {
    int index = carafeCatalogFindKey(g_catalog, CarafeFileKeys, "input-mode");
    if (index < 0) {
        return false;
    }
    const CarafeSetting* setting = &g_catalog->settings[index];
    return setting->current >= 0 && strcmp(setting->values[setting->current].text, "2") == 0;
}

static int stripItems(int* indices) {
    int count = 0;
    const CarafeSection* section = tabSection(g_tab);
    for (int i = 0; section != NULL && i < section->row_count && count < 4; i++) {
        if (section->rows[i].kind == CarafeRowSetting
            && carafeSettingVisible(g_catalog, &g_catalog->settings[section->rows[i].index])) {
            indices[count++] = section->rows[i].index;
        }
    }
    return count;
}

static int columnSources(int column, int* out) {
    int count = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < MAP[column][r].count; c++) {
            out[count++] = MAP[column][r].cells[c].source;
        }
    }
    return count;
}

static CarafeChord* chordOf(int source, bool pad) {
    return pad ? &g_catalog->pad_current[source] : &g_catalog->key_current[source];
}

static void openPicker(int source) {
    g_picker = true;
    g_picker_source = source;
    g_picker_pad = !keyboardMode();
    g_picker_chord = *chordOf(source, g_picker_pad);
    g_picker_focus = 0;
}

static int pickerCount(void) {
    return g_picker_pad ? g_catalog->pad_count : g_catalog->key_count;
}

static unsigned int pickerCode(int index) {
    return g_picker_pad ? g_catalog->pads[index].code : g_catalog->keys[index].code;
}

static void pickerToggle(int index) {
    if (index < 0 || index >= pickerCount()) {
        return;
    }
    unsigned int code = pickerCode(index);
    if (g_picker_pad) {
        g_picker_chord.count = 1;
        g_picker_chord.code[0] = (uint16_t)code;
        return;
    }
    for (int i = 0; i < g_picker_chord.count; i++) {
        if (g_picker_chord.code[i] == code) {
            memmove(&g_picker_chord.code[i], &g_picker_chord.code[i + 1],
                (size_t)(g_picker_chord.count - i - 1) * sizeof(uint16_t));
            g_picker_chord.count--;
            return;
        }
    }
    if (g_picker_chord.count < CARAFE_CHORD_MAX) {
        g_picker_chord.code[g_picker_chord.count++] = (uint16_t)code;
    }
}

static void pickerDone(void) {
    carafeBindingSet(g_catalog, g_picker_pad, g_picker_source, &g_picker_chord);
    g_picker = false;
}

static void closeLocked(void) {
    if (!g_open) {
        return;
    }
    g_open = false;
    carafeCatalogFree(g_catalog);
    g_catalog = NULL;
    regLeave();
    free(g_reg_entries);
    g_reg_entries = NULL;
    g_reg_count = 0;
    carafeRegistryClose();
    pthread_mutex_lock(&g_picture_lock);
    free(g_picture.pixels);
    g_picture.pixels = NULL;
    pthread_mutex_unlock(&g_picture_lock);
    free(g_icon.pixels);
    g_icon.pixels = NULL;
    carafeDrawRelease();
    g_rendered = 0;
    changed();
    note("[CARAFE] settings menu closed");
}

static bool openLocked(bool error_mode) {
    if (g_open) {
        return true;
    }
    g_catalog = carafeCatalogLoad();
    if (g_catalog == NULL) {
        return false;
    }
    g_language = carafeLanguage();
    g_error_mode = error_mode;
    g_tab = error_mode && g_failure != CarafeKindNone ? 0 : (error_mode ? 1 : 0);
    g_focus = g_scroll = 0;
    g_group = -1;
    g_picker = false;
    g_confirm = false;
    g_reset_armed = false;
    g_reg = false;
    g_edit = false;
    g_reg_available = !error_mode && carafeRegistryOpen();
    g_pad_focus = STRIP_MAX;
    g_held = error_mode ? OSK_A : OSK_MINUS | OSK_PLUS | OSK_L | OSK_R;
    g_repeat_bit = 0;
    g_touching = true;
    if (!carafeOverlayReadRomfsText("/carafe/title", g_title, sizeof(g_title))) {
        snprintf(g_title, sizeof(g_title), "Windows");
    }
    g_title[strcspn(g_title, "\r\n")] = '\0';
    carafeLoadIcon(&g_icon);
    g_open = true;
    changed();
    note("[CARAFE] settings menu opened%s", error_mode ? " from the error screen" : "");
    return true;
}

static void moveFocus(int delta, int count) {
    int* focus = focusRef();
    if (count <= 0) {
        *focus = 0;
        return;
    }
    *focus += delta;
    if (*focus < 0) {
        *focus = 0;
    }
    if (*focus >= count) {
        *focus = count - 1;
    }
    g_reset_armed = false;
}

static void switchTab(int delta) {
    if (g_group >= 0) {
        const CarafeGroup* group = &g_catalog->groups[g_group];
        if (group->page_count > 1) {
            g_page = (g_page + delta + group->page_count) % group->page_count;
            g_group_focus = 0;
            g_group_scroll = 0;
        }
        return;
    }
    int count = tabCount();
    g_tab = (g_tab + delta + count) % count;
    g_focus = 0;
    g_scroll = 0;
    g_pad_focus = STRIP_MAX;
    g_reset_armed = false;
}

static void undoRegistry(void) {
    if (g_reg_available) {
        carafeRegistryUndoPending();
        regRefresh();
    }
}

static const char* const KEYS_DEC[] = { "1", "2", "3", "4", "5", "←", "6", "7", "8", "9", "0", "CLR" };
static const char* const KEYS_HEX[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E",
    "F", "←", "CLR" };
static const char* const KEYS_TEXT[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "q", "w", "e", "r", "t",
    "y", "u", "i", "o", "p", "a", "s", "d", "f", "g", "h", "j", "k", "l", "_", "z", "x", "c", "v", "b", "n", "m", ".",
    "-", "\\", "Aa", "SPACE", ":", "/", "←", "CLR" };

static bool editOnType(void) {
    return g_edit_new && g_edit_field == 1;
}

static bool editNumberGrid(void) {
    return !(g_edit_new && g_edit_field == 0) && regNumber(g_edit_type);
}

static int editKeys(const char* const** keys, int* columns) {
    if (editNumberGrid()) {
        *keys = g_edit_hex ? KEYS_HEX : KEYS_DEC;
        *columns = 6;
        return g_edit_hex ? (int)(sizeof(KEYS_HEX) / sizeof(*KEYS_HEX)) : (int)(sizeof(KEYS_DEC) / sizeof(*KEYS_DEC));
    }
    *keys = KEYS_TEXT;
    *columns = 10;
    return (int)(sizeof(KEYS_TEXT) / sizeof(*KEYS_TEXT));
}

static char* editTarget(void) {
    return g_edit_new && g_edit_field == 0 ? g_edit_name : g_edit_text;
}

static void editErase(void) {
    char* target = editTarget();
    size_t length = strlen(target);
    while (length > 0 && (target[length - 1] & 0xC0) == 0x80) {
        length--;
    }
    if (length > 0) {
        length--;
    }
    target[length] = '\0';
}

static void editToggleBase(void) {
    if (!regNumber(g_edit_type) || (g_edit_new && g_edit_field != 2)) {
        return;
    }
    unsigned long long value = g_edit_text[0] != '\0' ? strtoull(g_edit_text, NULL, g_edit_hex ? 16 : 10) : 0;
    g_edit_hex = !g_edit_hex;
    if (g_edit_text[0] != '\0') {
        snprintf(g_edit_text, sizeof(g_edit_text), g_edit_hex ? "%llX" : "%llu", value);
    }
    g_edit_key = 0;
}

static void editPress(int index) {
    const char* const* keys;
    int columns;
    int count = editKeys(&keys, &columns);
    if (index < 0 || index >= count) {
        return;
    }
    g_edit_key = index;
    g_edit_error = NULL;
    const char* key = keys[index];
    char* target = editTarget();
    size_t length = strlen(target);
    if (strcmp(key, "←") == 0) {
        editErase();
        return;
    }
    if (strcmp(key, "CLR") == 0) {
        target[0] = '\0';
        return;
    }
    if (strcmp(key, "Aa") == 0) {
        g_edit_shift = !g_edit_shift;
        return;
    }
    const char* add = strcmp(key, "SPACE") == 0 ? " " : key;
    char upper[2] = { 0, 0 };
    if (g_edit_shift && add[0] >= 'a' && add[0] <= 'z' && add[1] == '\0') {
        upper[0] = (char)(add[0] - 'a' + 'A');
        add = upper;
    }
    size_t limit = editNumberGrid() ? (g_edit_hex ? (g_edit_type == CARAFE_REG_DWORD ? 8 : 16) : 20) : REG_TEXT_MAX;
    if (length + strlen(add) <= limit && length + strlen(add) < CARAFE_REG_NAME) {
        snprintf(target + length, CARAFE_REG_NAME - length, "%s", add);
    }
}

static void editSetType(uint32_t type) {
    if (g_edit_type != type) {
        g_edit_type = type;
        g_edit_text[0] = '\0';
        g_edit_hex = false;
    }
}

static CarafeMenuAction pressEditor(unsigned int down) {
    if (down & OSK_B) {
        g_edit = false;
        return CarafeMenuStay;
    }
    if (down & OSK_PLUS) {
        regEditCommit();
        return CarafeMenuStay;
    }
    if (down & OSK_X) {
        editErase();
        return CarafeMenuStay;
    }
    if (down & OSK_Y) {
        editToggleBase();
        return CarafeMenuStay;
    }
    if (g_edit_new && down & (OSK_L | OSK_R)) {
        g_edit_field = (g_edit_field + (down & OSK_L ? 2 : 1)) % 3;
        g_edit_key = 0;
        return CarafeMenuStay;
    }
    if (editOnType()) {
        if (down & (OSK_LEFT | OSK_RIGHT | OSK_A)) {
            editSetType(g_edit_type == CARAFE_REG_DWORD ? CARAFE_REG_SZ : CARAFE_REG_DWORD);
        } else if (down & OSK_DOWN) {
            g_edit_field = 2;
        } else if (down & OSK_UP) {
            g_edit_field = 0;
        }
        return CarafeMenuStay;
    }
    const char* const* keys;
    int columns;
    int count = editKeys(&keys, &columns);
    if (g_edit_key >= count) {
        g_edit_key = count - 1;
    }
    if (down & OSK_A) {
        editPress(g_edit_key);
    } else if (down & OSK_LEFT && g_edit_key > 0) {
        g_edit_key--;
    } else if (down & OSK_RIGHT && g_edit_key < count - 1) {
        g_edit_key++;
    } else if (down & OSK_UP) {
        if (g_edit_key >= columns) {
            g_edit_key -= columns;
        } else if (g_edit_new) {
            g_edit_field = g_edit_field == 2 ? 1 : 0;
        }
    } else if (down & OSK_DOWN && g_edit_key + columns < count) {
        g_edit_key += columns;
    } else if (down & OSK_DOWN && g_edit_key / columns < (count - 1) / columns) {
        g_edit_key = count - 1;
    }
    return CarafeMenuStay;
}

static CarafeMenuAction pressRegistry(unsigned int down) {
    if (g_edit) {
        return pressEditor(down);
    }
    if (down & OSK_B) {
        if (g_reg_view == 1 && g_reg_hive >= 0) {
            regAscend();
        } else {
            regLeave();
        }
        return CarafeMenuStay;
    }
    if (down & OSK_PLUS) {
        if (pendingTotal() > 0) {
            g_confirm = true;
            g_confirm_focus = 0;
        }
        return CarafeMenuStay;
    }
    if (down & (OSK_L | OSK_R)) {
        if (regHasRows()) {
            regShowView(g_reg_view == 0 ? 1 : 0);
        }
        return CarafeMenuStay;
    }
    int count = regListCount();
    int* focus = regFocus();
    if (*focus >= count) {
        *focus = count > 0 ? count - 1 : 0;
    }
    if (down & OSK_UP && *focus > 0) {
        (*focus)--;
        g_reg_armed = false;
    } else if (down & OSK_DOWN && *focus < count - 1) {
        (*focus)++;
        g_reg_armed = false;
    } else if (count > 0 && down & OSK_A) {
        regActivate(*focus, 1);
    } else if (count > 0 && g_reg_view == 0 && down & OSK_RIGHT) {
        regActivate(*focus, 1);
    } else if (count > 0 && g_reg_view == 0 && down & OSK_LEFT) {
        regActivate(*focus, -1);
    } else if (count > 0 && down & OSK_Y) {
        if (g_reg_view == 0) {
            carafeRegistryRowReset(*focus);
        } else if (g_reg_hive >= 0 && !g_reg_entries[*focus].key) {
            carafeRegistryDropPending(g_reg_hive, g_reg_path, g_reg_entries[*focus].name);
            regRefresh();
        }
    } else if (down & OSK_X && g_reg_view == 1 && g_reg_hive >= 0 && !g_reg_missing) {
        regOpenEditor(NULL);
    } else if (count > 0 && down & OSK_MINUS && g_reg_view == 1 && g_reg_hive >= 0
        && !g_reg_entries[*focus].key && g_reg_entries[*focus].state != CarafeRegDeleted) {
        if (g_reg_armed) {
            carafeRegistryDeletePending(g_reg_hive, g_reg_path, g_reg_entries[*focus].name);
            g_reg_armed = false;
            regRefresh();
        } else {
            g_reg_armed = true;
        }
    }
    return CarafeMenuStay;
}

static CarafeMenuAction pressConfirm(unsigned int down) {
    if (down & OSK_B) {
        g_confirm = false;
        return CarafeMenuStay;
    }
    if (down & OSK_X) {
        g_trial = !g_trial;
    }
    if (down & OSK_LEFT && g_confirm_focus > 0) {
        g_confirm_focus--;
    }
    if (down & OSK_RIGHT && g_confirm_focus < 2) {
        g_confirm_focus++;
    }
    if (down & OSK_A) {
        g_confirm = false;
        if (g_confirm_focus == 0) {
            return CarafeMenuRestart;
        }
        if (g_confirm_focus == 1) {
            return CarafeMenuBack;
        }
        carafeUndoPending(g_catalog);
        undoRegistry();
    }
    return CarafeMenuStay;
}

static CarafeMenuAction pressPicker(unsigned int down) {
    const int columns = g_picker_pad ? 4 : 8;
    int count = pickerCount();
    if (down & OSK_B) {
        g_picker = false;
    } else if (down & OSK_PLUS) {
        pickerDone();
    } else if (down & OSK_Y) {
        g_picker_chord.count = 0;
    } else if (down & OSK_A) {
        pickerToggle(g_picker_focus);
        if (g_picker_pad) {
            pickerDone();
        }
    } else if (down & OSK_LEFT && g_picker_focus > 0) {
        g_picker_focus--;
    } else if (down & OSK_RIGHT && g_picker_focus < count - 1) {
        g_picker_focus++;
    } else if (down & OSK_UP && g_picker_focus >= columns) {
        g_picker_focus -= columns;
    } else if (down & OSK_DOWN && g_picker_focus + columns < count) {
        g_picker_focus += columns;
    }
    return CarafeMenuStay;
}

static void pressPad(unsigned int down) {
    int strip[4];
    int strips = stripItems(strip);
    if (g_pad_focus < STRIP_MAX) {
        if (g_pad_focus >= strips) {
            g_pad_focus = strips > 0 ? strips - 1 : STRIP_MAX;
        }
        if (down & OSK_LEFT && g_pad_focus > 0) {
            g_pad_focus--;
        } else if (down & OSK_RIGHT && g_pad_focus < strips - 1) {
            g_pad_focus++;
        } else if (down & OSK_DOWN) {
            g_pad_focus = STRIP_MAX;
        } else if (down & OSK_A) {
            stepSetting(strip[g_pad_focus], 1);
        } else if (down & OSK_Y) {
            Item item = { ItemSetting, strip[g_pad_focus], 0 };
            resetItem(&item);
        }
        return;
    }
    int index = g_pad_focus - STRIP_MAX;
    int left[16];
    int right[16];
    int left_count = columnSources(0, left);
    int right_count = columnSources(1, right);
    int column = index >= left_count ? 1 : 0;
    int position = column ? index - left_count : index;
    if (down & OSK_UP) {
        if (position == 0) {
            g_pad_focus = 0;
            return;
        }
        position--;
    } else if (down & OSK_DOWN) {
        position++;
    } else if (down & (OSK_LEFT | OSK_RIGHT)) {
        column = down & OSK_LEFT ? 0 : 1;
    } else if (down & OSK_A) {
        openPicker(column ? right[position] : left[position]);
        return;
    }
    int limit = column ? right_count : left_count;
    if (position >= limit) {
        position = limit - 1;
    }
    g_pad_focus = STRIP_MAX + (column ? left_count + position : position);
}

static CarafeMenuAction pressList(unsigned int down) {
    Item items[MAX_ITEMS];
    int count = buildItems(items);
    int* focus = focusRef();
    if (*focus >= count) {
        *focus = count > 0 ? count - 1 : 0;
    }
    if (down & OSK_UP) {
        moveFocus(-1, count);
    } else if (down & OSK_DOWN) {
        moveFocus(1, count);
    } else if (count > 0 && down & OSK_A) {
        activateItem(&items[*focus], 1);
    } else if (count > 0 && down & OSK_RIGHT && items[*focus].kind != ItemGroup && items[*focus].kind != ItemReset
        && items[*focus].kind != ItemRegistry) {
        activateItem(&items[*focus], 1);
    } else if (count > 0 && down & OSK_LEFT && items[*focus].kind != ItemGroup && items[*focus].kind != ItemReset
        && items[*focus].kind != ItemRegistry) {
        activateItem(&items[*focus], -1);
    } else if (count > 0 && down & OSK_Y) {
        resetItem(&items[*focus]);
    }
    return CarafeMenuStay;
}

static CarafeMenuAction pressKeys(unsigned int down) {
    if (g_confirm) {
        return pressConfirm(down);
    }
    if (g_picker) {
        return pressPicker(down);
    }
    if (g_reg) {
        return pressRegistry(down);
    }
    if (down & OSK_B) {
        if (g_group >= 0) {
            g_group = -1;
            return CarafeMenuStay;
        }
        return CarafeMenuBack;
    }
    if (down & OSK_PLUS) {
        if (g_error_mode) {
            return CarafeMenuRestart;
        }
        if (pendingTotal() > 0) {
            g_confirm = true;
            g_confirm_focus = 0;
        }
        return CarafeMenuStay;
    }
    if (down & OSK_L) {
        switchTab(-1);
        return CarafeMenuStay;
    }
    if (down & OSK_R) {
        switchTab(1);
        return CarafeMenuStay;
    }
    if (g_group < 0 && tabIsControls(g_tab)) {
        pressPad(down);
        return CarafeMenuStay;
    }
    return pressList(down);
}

static int hitAt(int x, int y) {
    for (int i = g_hit_count - 1; i >= 0; i--) {
        const Hit* hit = &g_hits[i];
        if (x >= hit->x && x < hit->x + hit->w && y >= hit->y && y < hit->y + hit->h) {
            return i;
        }
    }
    return -1;
}

static CarafeMenuAction pressButton(int id) {
    switch (id) {
    case ButtonRestart:
        return pressKeys(OSK_PLUS);
    case ButtonGame:
        return CarafeMenuBack;
    case ButtonBack:
        return pressKeys(OSK_B);
    case ButtonEditDone:
        regEditCommit();
        return CarafeMenuStay;
    case ButtonEditCancel:
        g_edit = false;
        return CarafeMenuStay;
    case ButtonDone:
        pickerDone();
        return CarafeMenuStay;
    case ButtonClear:
        g_picker_chord.count = 0;
        return CarafeMenuStay;
    case ButtonConfirm:
        g_confirm = false;
        return CarafeMenuRestart;
    case ButtonLater:
        g_confirm = false;
        return CarafeMenuBack;
    case ButtonUndo:
        g_confirm = false;
        carafeUndoPending(g_catalog);
        undoRegistry();
        return CarafeMenuStay;
    case ButtonTrial:
        g_trial = !g_trial;
        return CarafeMenuStay;
    }
    return CarafeMenuStay;
}

static CarafeMenuAction tap(int x, int y) {
    int index = hitAt(x, y);
    if (index < 0) {
        return CarafeMenuStay;
    }
    Hit hit = g_hits[index];
    switch (hit.kind) {
    case HitTab:
        if (g_group < 0 && !g_picker && !g_confirm) {
            g_tab = hit.value;
            g_focus = g_scroll = 0;
            g_pad_focus = STRIP_MAX;
        }
        break;
    case HitPage:
        g_page = hit.value;
        g_group_focus = g_group_scroll = 0;
        break;
    case HitRow: {
        Item items[MAX_ITEMS];
        int count = buildItems(items);
        if (hit.value < count) {
            if (*focusRef() == hit.value) {
                activateItem(&items[hit.value], 1);
            } else {
                *focusRef() = hit.value;
                g_reset_armed = false;
            }
        }
        break;
    }
    case HitButton:
        return pressButton(hit.value);
    case HitStrip: {
        int strip[4];
        int strips = stripItems(strip);
        if (hit.value < strips) {
            if (g_pad_focus == hit.value) {
                stepSetting(strip[hit.value], 1);
            }
            g_pad_focus = hit.value;
        }
        break;
    }
    case HitSource:
        if (g_pad_focus == hit.value) {
            int left[16];
            int right[16];
            int left_count = columnSources(0, left);
            columnSources(1, right);
            int position = hit.value - STRIP_MAX;
            openPicker(position < left_count ? left[position] : right[position - left_count]);
        } else {
            g_pad_focus = hit.value;
        }
        break;
    case HitKey:
        g_picker_focus = hit.value;
        pickerToggle(hit.value);
        if (g_picker_pad) {
            pickerDone();
        }
        break;
    case HitRegChip:
        if (hit.value != g_reg_view) {
            regShowView(hit.value);
        }
        break;
    case HitRegRow:
        if (*regFocus() == hit.value) {
            regActivate(hit.value, 1);
        } else {
            *regFocus() = hit.value;
            g_reg_armed = false;
        }
        break;
    case HitRegKey:
        editPress(hit.value);
        break;
    case HitRegField:
        if (hit.value == 1 && g_edit_field == 1) {
            editSetType(g_edit_type == CARAFE_REG_DWORD ? CARAFE_REG_SZ : CARAFE_REG_DWORD);
        } else if (hit.value == 3) {
            editToggleBase();
        } else {
            g_edit_field = hit.value;
            g_edit_key = 0;
        }
        break;
    }
    return CarafeMenuStay;
}

CarafeMenuAction carafeMenuInput(unsigned int held, int touching, int touch_x, int touch_y, uint64_t now_ns) {
    pthread_mutex_lock(&g_lock);
    if (!g_open || g_catalog == NULL) {
        pthread_mutex_unlock(&g_lock);
        return CarafeMenuStay;
    }
    unsigned int down = held & ~g_held;
    const unsigned int repeats = OSK_UP | OSK_DOWN | OSK_LEFT | OSK_RIGHT;
    if (down & repeats) {
        g_repeat_bit = down & repeats;
        g_repeat_at = now_ns + REPEAT_FIRST_NS;
    } else if (g_repeat_bit && (held & g_repeat_bit) == g_repeat_bit && now_ns >= g_repeat_at) {
        down |= g_repeat_bit;
        g_repeat_at = now_ns + REPEAT_NEXT_NS;
    } else if ((held & g_repeat_bit) != g_repeat_bit) {
        g_repeat_bit = 0;
    }
    g_held = held;
    CarafeMenuAction action = CarafeMenuStay;
    if (down != 0) {
        action = pressKeys(down);
        changed();
    }
    if (touching && !g_touching) {
        CarafeMenuAction tapped = tap(touch_x, touch_y);
        if (tapped != CarafeMenuStay) {
            action = tapped;
        }
        changed();
    }
    g_touching = touching;
    pthread_mutex_unlock(&g_lock);
    return action;
}

static void drawGlyph(CarafeImage* picture, int x, int y, const char* label, uint32_t fill, uint32_t ink) {
    int width = carafeTextWidth(label, 12, CarafeHeavy);
    int w = width + 12 < 24 ? 24 : width + 12;
    carafeDrawRound(picture, x, y, w, 24, 12, fill, 255);
    CarafeTextStyle style = { 12, CarafeHeavy, 24, ink, true };
    carafeDrawLine(picture, label, &style, x, y, w);
}

static int drawHint(CarafeImage* picture, int x, int y, const char* key, const char* label) {
    drawGlyph(picture, x, y, key, CARAFE_COLOR_PRIMARY_INK, CARAFE_COLOR_INK);
    int glyph = carafeTextWidth(key, 12, CarafeHeavy) + 12;
    glyph = glyph < 24 ? 24 : glyph;
    CarafeTextStyle style = { 14, CarafeRegular, 24, CARAFE_COLOR_HINT, false };
    carafeDrawLine(picture, label, &style, x + glyph + 6, y, 0);
    return x + glyph + 6 + carafeTextWidth(label, 14, CarafeRegular) + 18;
}

static void addHit(HitKind kind, int value, int x, int y, int w, int h) {
    if (g_hit_count < MAX_HITS) {
        g_hits[g_hit_count++] = (Hit){ kind, value, x, y, w, h };
    }
}

static int drawButtonAt(CarafeImage* picture, int x, const char* glyph, const char* label, int style, ButtonId id) {
    int glyph_w = glyph != NULL ? 34 + 12 : 0;
    int width = (glyph != NULL ? 14 : 30) + glyph_w + carafeTextWidth(label, 22, CarafeBold) + 30;
    uint32_t fill = style == 1 ? CARAFE_COLOR_INK : style == 2 ? CARAFE_COLOR_RESTART : CARAFE_COLOR_WHITE;
    uint32_t ink = style == 1 ? CARAFE_COLOR_PRIMARY_INK : style == 2 ? CARAFE_COLOR_RESTART_INK : CARAFE_COLOR_INK;
    if (style == 0) {
        carafeDrawRound(picture, x, BUTTONS_Y, width, BUTTON_H, 16, CARAFE_COLOR_WHITE, 18);
        carafeDrawRoundStroke(picture, x, BUTTONS_Y, width, BUTTON_H, 16, 2, CARAFE_COLOR_WHITE, 36);
    } else {
        carafeDrawRound(picture, x, BUTTONS_Y, width, BUTTON_H, 16, fill, 255);
    }
    int text_x = x + (glyph != NULL ? 14 : 30);
    if (glyph != NULL) {
        uint32_t glyph_fill = style == 2 ? CARAFE_COLOR_RESTART_INK : CARAFE_COLOR_PRIMARY_INK;
        uint32_t glyph_ink = style == 2 ? CARAFE_COLOR_RESTART : CARAFE_COLOR_INK;
        carafeDrawCircle(picture, text_x + 17, BUTTONS_Y + BUTTON_H / 2, 17, glyph_fill, 255);
        CarafeTextStyle g = { 19, CarafeHeavy, 34, glyph_ink, true };
        carafeDrawLine(picture, glyph, &g, text_x, BUTTONS_Y + (BUTTON_H - 34) / 2, 34);
        text_x += glyph_w;
    }
    CarafeTextStyle s = { 22, CarafeBold, BUTTON_H, ink, false };
    carafeDrawLine(picture, label, &s, text_x, BUTTONS_Y, 0);
    addHit(HitButton, id, x, BUTTONS_Y, width, BUTTON_H);
    return x + width + 16;
}

static void drawHeader(CarafeImage* picture) {
    const Strings* s = text();
    int x = LIST_X;
    if (g_icon.pixels != NULL) {
        carafeDrawPicture(picture, &g_icon, x, 36, 48, 12);
    } else {
        carafeDrawRound(picture, x, 36, 48, 48, 12, CARAFE_COLOR_ACCENT, 255);
    }
    x += 64;
    CarafeTextStyle name = { 21, CarafeBold, 26, CARAFE_COLOR_INK, false };
    carafeDrawLine(picture, g_title, &name, x, 38, 0);
    char subline[TEXT_SIZE];
    int pending = pendingTotal();
    if (g_error_mode) {
        snprintf(subline, sizeof(subline), "%s", s->failed);
    } else if (pending > 0) {
        snprintf(subline, sizeof(subline), s->pending[carafePluralForm(pending)], pending);
    } else {
        snprintf(subline, sizeof(subline), "%s", s->running);
    }
    CarafeTextStyle sub = { 14, CarafeRegular, 20, pending > 0 && !g_error_mode ? CARAFE_COLOR_RESTART : CARAFE_COLOR_LABEL,
        false };
    carafeDrawLine(picture, subline, &sub, x, 64, 0);

    if (g_group >= 0 || g_picker || g_reg) {
        int bx = 1208;
        const CarafeSection* section = tabSection(g_tab);
        const char* parent = section != NULL ? pick(section->name) : "";
        char title[TEXT_SIZE];
        if (g_reg) {
            snprintf(title, sizeof(title), "%s", reg()->title);
        } else if (g_picker) {
            snprintf(title, sizeof(title), s->button, carafe_source_names[g_picker_source]);
        } else {
            snprintf(title, sizeof(title), "%s", pick(g_catalog->groups[g_group].name));
        }
        int title_w = carafeTextWidth(title, 20, CarafeBold);
        bx -= title_w;
        carafeDrawText(picture, title, 20, CarafeBold, bx, 45, CARAFE_COLOR_INK);
        char crumb[TEXT_SIZE];
        snprintf(crumb, sizeof(crumb), "%s ›", parent);
        int crumb_w = carafeTextWidth(crumb, 17, CarafeRegular);
        bx -= crumb_w + 10;
        carafeDrawText(picture, crumb, 17, CarafeRegular, bx, 48, CARAFE_COLOR_HINT);
        const char* back = g_reg && g_reg_view == 1 && g_reg_hive >= 0 ? reg()->up : s->back;
        int back_w = 24 + 8 + carafeTextWidth(back, 15, CarafeBold) + 18;
        bx -= back_w + 14;
        carafeDrawRound(picture, bx, 42, back_w, 36, 12, CARAFE_COLOR_WHITE, 18);
        drawGlyph(picture, bx + 6, 48, "B", CARAFE_COLOR_PRIMARY_INK, CARAFE_COLOR_INK);
        carafeDrawText(picture, back, 15, CarafeBold, bx + 38, 50, CARAFE_COLOR_INK);
        addHit(HitButton, ButtonBack, bx, 42, back_w, 36);
        return;
    }
    int count = tabCount();
    int widths[16];
    int total = 34 + 8 + 34;
    for (int i = 0; i < count && i < 16; i++) {
        const char* label = tabIsTry(i) ? s->what_to_try : pick(tabSection(i)->name);
        widths[i] = carafeTextWidth(label, 16, CarafeBold) + 28;
        total += widths[i] + 4;
    }
    int tx = 1208 - total;
    carafeDrawRoundStroke(picture, tx, 43, 34, 26, 8, 2, CARAFE_COLOR_DIM, 255);
    CarafeTextStyle shoulder = { 13, CarafeHeavy, 26, CARAFE_COLOR_HINT, true };
    carafeDrawLine(picture, "L", &shoulder, tx, 43, 34);
    tx += 34 + 4;
    for (int i = 0; i < count && i < 16; i++) {
        const char* label = tabIsTry(i) ? s->what_to_try : pick(tabSection(i)->name);
        bool on = i == g_tab;
        if (on) {
            carafeDrawRound(picture, tx, 36, widths[i], 40, 12, CARAFE_COLOR_FOCUS, 255);
        }
        CarafeTextStyle style = { 16, CarafeBold, 40, on ? 0xFF22121Au : CARAFE_COLOR_HINT, true };
        carafeDrawLine(picture, label, &style, tx, 36, widths[i]);
        addHit(HitTab, i, tx, 36, widths[i], 40);
        tx += widths[i] + 4;
    }
    carafeDrawRoundStroke(picture, tx, 43, 34, 26, 8, 2, CARAFE_COLOR_DIM, 255);
    carafeDrawLine(picture, "R", &shoulder, tx, 43, 34);
}

static const char* itemName(const Item* item) {
    switch (item->kind) {
    case ItemSetting:
        return pick(g_catalog->settings[item->index].name);
    case ItemGroup:
        return pick(g_catalog->groups[item->index].name);
    case ItemPreset:
        return text()->preset;
    case ItemReset:
        return text()->reset_all;
    case ItemSuggest:
        return pick(g_catalog->settings[g_catalog->suggests[item->index].setting].name);
    case ItemRegistry:
        return reg()->title;
    }
    return "";
}

static void itemValue(const Item* item, char* out, size_t size) {
    out[0] = '\0';
    switch (item->kind) {
    case ItemSetting:
        snprintf(out, size, "%s", carafeValueLabel(&g_catalog->settings[item->index]));
        break;
    case ItemGroup:
        snprintf(out, size, "›");
        break;
    case ItemPreset: {
        const CarafeSection* section = tabSection(g_tab);
        int current = section != NULL ? carafePresetCurrent(g_catalog, section) : -1;
        snprintf(out, size, "%s", current >= 0 ? pick(section->presets[current].name) : text()->custom);
        break;
    }
    case ItemReset:
    case ItemRegistry:
        snprintf(out, size, "›");
        break;
    case ItemSuggest:
        snprintf(out, size, "%s", carafeValueLabel(&g_catalog->settings[g_catalog->suggests[item->index].setting]));
        break;
    }
}

static bool itemChanged(const Item* item) {
    if (item->kind == ItemSetting || item->kind == ItemSuggest) {
        int index = item->kind == ItemSetting ? item->index : g_catalog->suggests[item->index].setting;
        const CarafeSetting* setting = &g_catalog->settings[index];
        return setting->current != defaultIndex(setting);
    }
    return false;
}

static bool itemDimmed(const Item* item) {
    if (item->kind == ItemSetting) {
        return !settingActive(&g_catalog->settings[item->index]);
    }
    return false;
}

static void drawRow(CarafeImage* picture, int index, const Item* item, int y, bool focused) {
    char value[TEXT_SIZE];
    itemValue(item, value, sizeof(value));
    bool dim = itemDimmed(item);
    if (focused) {
        carafeDrawFocusRing(picture, LIST_X, y, LIST_W, ROW_H, 14);
    }
    uint32_t ink = focused ? CARAFE_COLOR_PRIMARY_INK : dim ? CARAFE_COLOR_DIM : CARAFE_COLOR_VALUE;
    if (itemChanged(item)) {
        carafeDrawCircle(picture, LIST_X + 18, y + ROW_H / 2, 4, CARAFE_COLOR_FOCUS, 255);
    }
    int value_w = carafeTextWidth(value, 16, CarafeBold);
    CarafeTextStyle name = { 16, CarafeRegular, ROW_H, ink, false };
    carafeDrawLine(picture, itemName(item), &name, LIST_X + 32, y, 0);
    CarafeTextStyle val = { 16, CarafeBold, ROW_H, focused ? CARAFE_COLOR_PRIMARY_INK : dim ? CARAFE_COLOR_DIM
                                                                                         : CARAFE_COLOR_INK,
        false };
    carafeDrawLine(picture, value, &val, LIST_X + LIST_W - 16 - value_w, y, 0);
    addHit(HitRow, index, LIST_X, y, LIST_W, ROW_H);
}

static void drawCardSetting(CarafeImage* picture, const CarafeSetting* setting, const char* why, int top, int bottom) {
    const Strings* s = text();
    int x = CARD_X + 30;
    int width = CARD_W - 60;
    CarafeTextStyle title = { 24, CarafeHeavy, 29, CARAFE_COLOR_INK, false };
    int y = carafeDrawWrapped(picture, pick(setting->name), &title, x, top + 26, width) + 8;
    CarafeTextStyle body = { 16, CarafeRegular, 24, CARAFE_COLOR_TEXT, false };
    if (why != NULL && why[0] != '\0') {
        CarafeTextStyle reason = { 16, CarafeBold, 24, CARAFE_COLOR_CHIP, false };
        y = carafeDrawWrapped(picture, why, &reason, x, y, width) + 4;
    }
    y = carafeDrawWrapped(picture, pick(setting->hint), &body, x, y, width);
    if (!settingActive(setting)) {
        CarafeTextStyle off = { 16, CarafeRegular, 24, CARAFE_COLOR_LABEL, false };
        y = carafeDrawWrapped(picture, s->inactive, &off, x, y, width);
    }
    y += 10;
    int def = defaultIndex(setting);
    int limit = bottom - 64;
    for (int i = 0; i < setting->value_count && y + 34 <= limit; i++) {
        bool current = i == setting->current;
        if (current) {
            carafeDrawRound(picture, x - 4, y, width + 8, 32, 10, CARAFE_COLOR_FOCUS, 41);
            carafeDrawRing(picture, x + 10, y + 16, 8, 5, CARAFE_COLOR_FOCUS, 255);
        } else {
            carafeDrawRing(picture, x + 10, y + 16, 8, 2, CARAFE_COLOR_DIM, 255);
        }
        CarafeTextStyle option = { 15, current ? CarafeBold : CarafeRegular, 32,
            current ? CARAFE_COLOR_INK : CARAFE_COLOR_TEXT, false };
        const char* label = pick(setting->values[i].label);
        carafeDrawLine(picture, label, &option, x + 28, y, 0);
        if (i == def) {
            int w = carafeTextWidth(s->default_mark, 12, CarafeRegular);
            carafeDrawText(picture, s->default_mark, 12, CarafeRegular, x + width - w, y + 8, CARAFE_COLOR_CHIP);
        }
        y += 34;
    }
    const char* mode = setting->now ? s->now : g_error_mode ? s->after_restart_error : s->after_restart;
    uint32_t color = setting->now ? CARAFE_COLOR_LIVE : CARAFE_COLOR_RESTART;
    int mode_h = carafeWrappedHeight(mode, &body, width - 28) + 16;
    carafeDrawRound(picture, x - 4, bottom - 26 - mode_h, width + 8, mode_h, 12, color, 30);
    CarafeTextStyle mode_style = { 14, CarafeRegular, 20, color, false };
    carafeDrawWrapped(picture, mode, &mode_style, x + 10, bottom - 26 - mode_h + 8, width - 28);
}

static void drawCard(CarafeImage* picture, const Item* item, int top) {
    int bottom = LIST_BOTTOM;
    carafeDrawRound(picture, CARD_X, top, CARD_W, bottom - top, 24, CARAFE_COLOR_PANEL, 128);
    carafeDrawRoundStroke(picture, CARD_X, top, CARD_W, bottom - top, 24, 1, CARAFE_COLOR_WHITE, 20);
    if (item == NULL) {
        return;
    }
    const Strings* s = text();
    int x = CARD_X + 30;
    int width = CARD_W - 60;
    if (item->kind == ItemSetting) {
        drawCardSetting(picture, &g_catalog->settings[item->index], NULL, top, bottom);
        return;
    }
    if (item->kind == ItemSuggest) {
        const CarafeSuggest* suggest = &g_catalog->suggests[item->index];
        drawCardSetting(picture, &g_catalog->settings[suggest->setting], pick(suggest->why), top, bottom);
        return;
    }
    CarafeTextStyle title = { 24, CarafeHeavy, 29, CARAFE_COLOR_INK, false };
    CarafeTextStyle body = { 16, CarafeRegular, 24, CARAFE_COLOR_TEXT, false };
    int y = carafeDrawWrapped(picture, itemName(item), &title, x, top + 26, width) + 8;
    if (item->kind == ItemPreset) {
        const CarafeSection* section = tabSection(g_tab);
        y = carafeDrawWrapped(picture, s->preset_hint, &body, x, y, width) + 10;
        int current = carafePresetCurrent(g_catalog, section);
        for (int i = 0; i < section->preset_count; i++) {
            bool on = i == current;
            if (on) {
                carafeDrawRound(picture, x - 4, y, width + 8, 32, 10, CARAFE_COLOR_FOCUS, 41);
            }
            carafeDrawRing(picture, x + 10, y + 16, 8, on ? 5 : 2, on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_DIM, 255);
            CarafeTextStyle option = { 15, on ? CarafeBold : CarafeRegular, 32, on ? CARAFE_COLOR_INK : CARAFE_COLOR_TEXT,
                false };
            carafeDrawLine(picture, pick(section->presets[i].name), &option, x + 28, y, 0);
            y += 34;
            y = carafeDrawWrapped(picture, pick(section->presets[i].hint),
                    &(CarafeTextStyle){ 13, CarafeRegular, 18, CARAFE_COLOR_LABEL, false }, x + 28, y, width - 28) + 6;
        }
    } else if (item->kind == ItemRegistry) {
        carafeDrawWrapped(picture, reg()->item_hint, &body, x, y, width);
    } else if (item->kind == ItemReset) {
        carafeDrawWrapped(picture, g_reset_armed ? s->reset_again : s->reset_all_hint, &body, x, y, width);
    } else if (item->kind == ItemGroup) {
        const CarafeGroup* group = &g_catalog->groups[item->index];
        for (int p = 0; p < group->page_count; p++) {
            char line[TEXT_SIZE];
            int visible = 0;
            for (int r = 0; r < group->pages[p].row_count; r++) {
                visible += carafeSettingVisible(g_catalog, &g_catalog->settings[group->pages[p].rows[r].index]);
            }
            snprintf(line, sizeof(line), "%s · %d", pick(group->pages[p].name), visible);
            y = carafeDrawWrapped(picture, line, &body, x, y, width);
        }
    }
}

static void drawList(CarafeImage* picture) {
    Item items[MAX_ITEMS];
    int count = buildItems(items);
    int top = LIST_TOP;
    if (g_group >= 0 && g_catalog->groups[g_group].page_count > 1) {
        const CarafeGroup* group = &g_catalog->groups[g_group];
        int x = LIST_X;
        for (int p = 0; p < group->page_count; p++) {
            const char* name = pick(group->pages[p].name);
            int w = carafeTextWidth(name, 14, CarafeBold) + 24;
            if (x + w > 1208) {
                break;
            }
            bool on = p == g_page;
            carafeDrawRound(picture, x, 104, w, 32, 16, on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_WHITE, on ? 255 : 18);
            CarafeTextStyle style = { 14, CarafeBold, 32, on ? 0xFF22121Au : CARAFE_COLOR_HINT, true };
            carafeDrawLine(picture, name, &style, x, 104, w);
            addHit(HitPage, p, x, 104, w, 32);
            x += w + 6;
        }
        top = LIST_TOP_PAGES;
    }
    int* focus = focusRef();
    int* scroll = scrollRef();
    if (*focus >= count) {
        *focus = count > 0 ? count - 1 : 0;
    }
    int visible = (LIST_BOTTOM - top) / (ROW_H + ROW_GAP);
    if (*focus < *scroll) {
        *scroll = *focus;
    }
    if (*focus >= *scroll + visible) {
        *scroll = *focus - visible + 1;
    }
    int y = top;
    for (int i = *scroll; i < count && i < *scroll + visible; i++) {
        drawRow(picture, i, &items[i], y + 3, i == *focus);
        y += ROW_H + ROW_GAP;
    }
    if (*scroll + visible < count) {
        carafeDrawText(picture, "…", 18, CarafeBold, LIST_X + LIST_W / 2, LIST_BOTTOM - 8, CARAFE_COLOR_LABEL);
    }
    drawCard(picture, count > 0 ? &items[*focus] : NULL, top);
}

static void drawDevicePart(CarafeImage* picture, int source, int x, int y, int w, int h, int radius) {
    bool on = false;
    int focused = g_pad_focus - STRIP_MAX;
    if (focused >= 0) {
        int left[16];
        int right[16];
        int left_count = columnSources(0, left);
        columnSources(1, right);
        int focus_source = focused < left_count ? left[focused] : right[focused - left_count];
        on = focus_source == source || ((source == S_STICKL)
                                           && (focus_source == S_LUP || focus_source == S_LDOWN
                                               || focus_source == S_LLEFT || focus_source == S_LRIGHT))
            || ((source == S_STICKR)
                && (focus_source == S_RUP || focus_source == S_RDOWN || focus_source == S_RLEFT
                    || focus_source == S_RRIGHT));
    }
    carafeDrawRound(picture, x, y, w, h, radius, on ? CARAFE_COLOR_FOCUS : 0xFF1C0B13u, 255);
    carafeDrawRoundStroke(picture, x, y, w, h, radius, 2, on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_TEXT, 255);
}

static void drawDevice(CarafeImage* picture) {
    const float k = 600.0f / 251.0f;
    const int ox = 340 + (int)(6 * k);
    const int oy = 210 + (int)(16 * k);
#define DX(v) (ox + (int)((v) * k))
#define DY(v) (oy + (int)((v) * k))
#define DS(v) ((int)((v) * k))
    carafeDrawRoundStroke(picture, DX(0), DY(0), DS(239), DS(102), DS(16), 2, CARAFE_COLOR_TEXT, 255);
    carafeDrawRound(picture, DX(35.9f), DY(0), 2, DS(102), 0, CARAFE_COLOR_TEXT, 255);
    carafeDrawRound(picture, DX(203.1f), DY(0), 2, DS(102), 0, CARAFE_COLOR_TEXT, 255);
    carafeDrawRoundStroke(picture, DX(43.5f), DY(8), DS(152), DS(86), 4, 1, CARAFE_COLOR_TEXT, 200);
    CarafeTextStyle mode = { 13, CarafeRegular, 20, CARAFE_COLOR_LABEL, true };
    carafeDrawLine(picture, keyboardMode() ? text()->keyboard : text()->gamepad, &mode, DX(43.5f), DY(47), DS(152));
    drawDevicePart(picture, S_ZL, DX(9), DY(-11.5f), DS(24.5f), DS(7.5f), DS(3.75f));
    drawDevicePart(picture, S_ZR, DX(205.5f), DY(-11.5f), DS(24.5f), DS(7.5f), DS(3.75f));
    drawDevicePart(picture, S_L, DX(6), DY(-2.5f), DS(28), DS(3), DS(1.5f));
    drawDevicePart(picture, S_R, DX(205), DY(-2.5f), DS(28), DS(3), DS(1.5f));
    drawDevicePart(picture, S_MINUS, DX(25.8f), DY(7.6f), DS(6.4f), DS(1.9f) + 2, 2);
    drawDevicePart(picture, S_PLUS, DX(207.5f), DY(6), DS(5), DS(5), 3);
    drawDevicePart(picture, S_STICKL, DX(17.5f - 8), DY(28 - 8), DS(16), DS(16), DS(8));
    drawDevicePart(picture, S_STICKR, DX(221.5f - 8), DY(60 - 8), DS(16), DS(16), DS(8));
    const int dpad[4][3] = { { S_UP, 0, -1 }, { S_DOWN, 0, 1 }, { S_LEFT, -1, 0 }, { S_RIGHT, 1, 0 } };
    for (int i = 0; i < 4; i++) {
        drawDevicePart(picture, dpad[i][0], DX(17.5f + dpad[i][1] * 9.2f - 4.2f), DY(60 + dpad[i][2] * 9.2f - 4.2f),
            DS(8.4f), DS(8.4f), DS(4.2f));
    }
    const int abxy[4][3] = { { S_X, 0, -1 }, { S_B, 0, 1 }, { S_Y, -1, 0 }, { S_A, 1, 0 } };
    const char* letters[4] = { "X", "B", "Y", "A" };
    for (int i = 0; i < 4; i++) {
        int bx = DX(221.5f + abxy[i][1] * 9.2f - 4.2f);
        int by = DY(28 + abxy[i][2] * 9.2f - 4.2f);
        drawDevicePart(picture, abxy[i][0], bx, by, DS(8.4f), DS(8.4f), DS(4.2f));
        CarafeTextStyle letter = { 12, CarafeHeavy, DS(8.4f), CARAFE_COLOR_INK, true };
        carafeDrawLine(picture, letters[i], &letter, bx, by, DS(8.4f));
    }
#undef DX
#undef DY
#undef DS
}

static void drawPad(CarafeImage* picture) {
    int strip[4];
    int strips = stripItems(strip);
    int width = strips > 0 ? (1136 - (strips - 1) * 10) / strips : 0;
    for (int i = 0; i < strips; i++) {
        const CarafeSetting* setting = &g_catalog->settings[strip[i]];
        int x = LIST_X + i * (width + 10);
        bool focus = g_pad_focus == i;
        if (focus) {
            carafeDrawFocusRing(picture, x, 104, width, 52, 14);
        } else {
            carafeDrawRound(picture, x, 104, width, 52, 14, CARAFE_COLOR_WHITE, 13);
            carafeDrawRoundStroke(picture, x, 104, width, 52, 14, 2, CARAFE_COLOR_WHITE, 26);
        }
        uint32_t ink = focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_VALUE;
        CarafeTextStyle name = { 15, CarafeRegular, 52, ink, false };
        carafeDrawLine(picture, pick(setting->name), &name, x + 14, 104, 0);
        const char* value = carafeValueLabel(setting);
        int vw = carafeTextWidth(value, 15, CarafeBold);
        CarafeTextStyle val = { 15, CarafeBold, 52, focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_INK, false };
        carafeDrawLine(picture, value, &val, x + width - 14 - vw, 104, 0);
        addHit(HitStrip, i, x, 104, width, 52);
    }
    bool pad = !keyboardMode();
    int left[16];
    int left_count = columnSources(0, left);
    for (int column = 0; column < 2; column++) {
        int x = column == 0 ? 72 : 958;
        int y = 176;
        int ordinal = column == 0 ? 0 : left_count;
        for (int r = 0; r < 5; r++) {
            const MapRow* row = &MAP[column][r];
            int h = row->cross ? 118 : 42;
            bool row_on = false;
            for (int c = 0; c < row->count; c++) {
                row_on |= g_pad_focus - STRIP_MAX == ordinal + c;
            }
            carafeDrawRound(picture, x, y, 250, h, 14, row_on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_PANEL,
                row_on ? 36 : 115);
            carafeDrawRoundStroke(picture, x, y, 250, h, 14, 1, row_on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_WHITE,
                row_on ? 255 : 20);
            carafeDrawText(picture, text()->rows[row->label][0], 13, CarafeBold, x + 10, y + (row->cross ? 4 : 5),
                CARAFE_COLOR_INK);
            carafeDrawText(picture, text()->rows[row->label][1], 11, CarafeRegular, x + 10, y + (row->cross ? 20 : 21),
                CARAFE_COLOR_LABEL);
            for (int c = 0; c < row->count; c++) {
                const Cell* cell = &row->cells[c];
                int cw = row->cross ? 76 : 120;
                int cx = row->cross ? x + 6 + cell->column * 80 : x + 124;
                int cy = row->cross ? y + 26 + cell->row * 30 : y + 7;
                bool focus = g_pad_focus - STRIP_MAX == ordinal + c;
                char label[TEXT_SIZE];
                carafeChordLabel(g_catalog, pad, chordOf(cell->source, pad), label, sizeof(label));
                if (focus) {
                    carafeDrawRound(picture, cx - 4, cy - 4, cw + 8, 36, 12, CARAFE_COLOR_FOCUS, 255);
                    carafeDrawRound(picture, cx - 2, cy - 2, cw + 4, 32, 11, CARAFE_COLOR_GAP, 255);
                    carafeDrawRound(picture, cx, cy, cw, 28, 9, CARAFE_COLOR_INK, 255);
                } else {
                    carafeDrawRound(picture, cx, cy, cw, 28, 9, 0xFF1C0B13u, 255);
                    carafeDrawRoundStroke(picture, cx, cy, cw, 28, 9, 1, CARAFE_COLOR_DIM, 255);
                }
                bool marked = cell->mark[0] != '\0' && strstr(label, cell->mark) == NULL;
                int mark_w = marked ? carafeTextWidth(cell->mark, 10, CarafeBold) + 8 : 0;
                if (marked) {
                    carafeDrawText(picture, cell->mark, 10, CarafeBold, cx + 6, cy + 7,
                        focus ? CARAFE_COLOR_DIM : CARAFE_COLOR_LABEL);
                }
                int size = 12;
                while (size > 9 && carafeTextWidth(label, size, CarafeBold) > cw - 8 - mark_w) {
                    size--;
                }
                CarafeTextStyle chip = { size, CarafeBold, 28, focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_VALUE,
                    true };
                carafeDrawLine(picture, label, &chip, cx + mark_w, cy, cw - mark_w);
                addHit(HitSource, STRIP_MAX + ordinal + c, cx, cy, cw, 28);
            }
            ordinal += row->count;
            y += h + 8;
        }
    }
    drawDevice(picture);
}

static void drawPicker(CarafeImage* picture) {
    const Strings* s = text();
    const int columns = g_picker_pad ? 4 : 8;
    const int cell_w = g_picker_pad ? 152 : 74;
    const int cell_h = 44;
    int count = pickerCount();
    int rows_visible = (LIST_BOTTOM - LIST_TOP) / (cell_h + 6);
    int focus_row = g_picker_focus / columns;
    int first_row = focus_row >= rows_visible ? focus_row - rows_visible + 1 : 0;
    for (int i = first_row * columns; i < count && i < (first_row + rows_visible) * columns; i++) {
        int column = i % columns;
        int row = i / columns - first_row;
        int x = LIST_X + column * (cell_w + 6);
        int y = LIST_TOP + row * (cell_h + 6);
        unsigned int code = pickerCode(i);
        bool chosen = false;
        for (int c = 0; c < g_picker_chord.count; c++) {
            chosen |= g_picker_chord.code[c] == code;
        }
        bool focus = i == g_picker_focus;
        if (focus) {
            carafeDrawFocusRing(picture, x, y, cell_w, cell_h, 10);
        } else {
            carafeDrawRound(picture, x, y, cell_w, cell_h, 10, chosen ? CARAFE_COLOR_ACCENT : CARAFE_COLOR_WHITE,
                chosen ? 255 : 20);
        }
        const CarafeKey* key = g_picker_pad ? &g_catalog->pads[i] : &g_catalog->keys[i];
        const char* label = pick(key->label);
        int size = 15;
        while (size > 9 && carafeTextWidth(label, size, CarafeBold) > cell_w - 6) {
            size--;
        }
        CarafeTextStyle style = { size, CarafeBold, cell_h, focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_VALUE, true };
        carafeDrawLine(picture, label, &style, x, y, cell_w);
        addHit(HitKey, i, x, y, cell_w, cell_h);
    }
    int top = LIST_TOP;
    carafeDrawRound(picture, CARD_X, top, CARD_W, LIST_BOTTOM - top, 24, CARAFE_COLOR_PANEL, 128);
    int x = CARD_X + 30;
    int width = CARD_W - 60;
    char title[TEXT_SIZE];
    snprintf(title, sizeof(title), s->button, carafe_source_names[g_picker_source]);
    CarafeTextStyle head = { 24, CarafeHeavy, 29, CARAFE_COLOR_INK, false };
    int y = carafeDrawWrapped(picture, title, &head, x, top + 26, width) + 8;
    CarafeTextStyle body = { 16, CarafeRegular, 24, CARAFE_COLOR_TEXT, false };
    y = carafeDrawWrapped(picture, g_picker_pad ? s->pick_pad : s->pick_keys, &body, x, y, width) + 12;
    char chord[TEXT_SIZE];
    carafeChordLabel(g_catalog, g_picker_pad, &g_picker_chord, chord, sizeof(chord));
    int cw = carafeTextWidth(chord, 16, CarafeBold) + 28;
    carafeDrawRound(picture, x, y, cw > width ? width : cw, 34, 17, CARAFE_COLOR_FOCUS, 41);
    CarafeTextStyle chip = { 16, CarafeBold, 34, CARAFE_COLOR_CHIP, true };
    carafeDrawLine(picture, chord, &chip, x, y, cw > width ? width : cw);
    int mode_h = 40;
    carafeDrawRound(picture, x - 4, LIST_BOTTOM - 26 - mode_h, width + 8, mode_h, 12, CARAFE_COLOR_LIVE, 30);
    CarafeTextStyle mode = { 14, CarafeRegular, mode_h, CARAFE_COLOR_LIVE, false };
    carafeDrawLine(picture, s->live_controls, &mode, x + 10, LIST_BOTTOM - 26 - mode_h, 0);
}

static void fitText(const char* text, int size, CarafeWeight weight, int max, char* out, size_t out_size) {
    snprintf(out, out_size, "%s", text);
    if (carafeTextWidth(out, size, weight) <= max) {
        return;
    }
    size_t length = strlen(out);
    while (length > 0) {
        length--;
        while (length > 0 && (out[length] & 0xC0) == 0x80) {
            length--;
        }
        snprintf(out + length, out_size - length, "…");
        if (carafeTextWidth(out, size, weight) <= max) {
            return;
        }
    }
}

static const char* regTypeTag(uint32_t type) {
    if (regNumber(type)) {
        return reg()->number;
    }
    if (type == CARAFE_REG_SZ || type == CARAFE_REG_EXPAND_SZ || type == CARAFE_REG_MULTI_SZ) {
        return reg()->string;
    }
    return "HEX";
}

static const char* regTypeName(uint32_t type, char* out, size_t size) {
    const RegStrings* r = reg();
    const uint32_t types[6] = { CARAFE_REG_DWORD, CARAFE_REG_QWORD, CARAFE_REG_SZ, CARAFE_REG_EXPAND_SZ,
        CARAFE_REG_MULTI_SZ, CARAFE_REG_BINARY };
    for (int i = 0; i < 6; i++) {
        if (types[i] == type) {
            return r->type_names[i];
        }
    }
    snprintf(out, size, r->type_other, type);
    return out;
}

static void drawTag(CarafeImage* picture, int right, int y, const char* label, bool focused, bool strong) {
    int w = carafeTextWidth(label, 11, CarafeBold) + 16;
    uint32_t fill = focused ? 0xFFEAD7DEu : strong ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_WHITE;
    unsigned int alpha = focused ? 255 : strong ? 46 : 20;
    carafeDrawRound(picture, right - w, y + (ROW_H - 20) / 2, w, 20, 10, fill, alpha);
    CarafeTextStyle style = { 11, CarafeBold, 20, focused ? CARAFE_COLOR_DIM : strong ? CARAFE_COLOR_CHIP
                                                                                      : CARAFE_COLOR_HINT,
        true };
    carafeDrawLine(picture, label, &style, right - w, y + (ROW_H - 20) / 2, w);
}

static void drawFolder(CarafeImage* picture, int x, int y, uint32_t color) {
    carafeDrawRoundStroke(picture, x, y + 4, 18, 13, 3, 2, color, 255);
    carafeDrawRound(picture, x, y + 1, 8, 5, 2, color, 255);
}

static void drawRegRow(CarafeImage* picture, int index, int y, bool focused, const char* name, const char* value,
    const char* tag, bool strong_tag, bool dot, bool dim, bool folder) {
    if (focused) {
        carafeDrawFocusRing(picture, LIST_X, y, LIST_W, ROW_H, 14);
    }
    uint32_t ink = focused ? CARAFE_COLOR_PRIMARY_INK : dim ? CARAFE_COLOR_DIM : CARAFE_COLOR_VALUE;
    if (dot) {
        carafeDrawCircle(picture, LIST_X + 18, y + ROW_H / 2, 4, CARAFE_COLOR_FOCUS, 255);
    }
    int x = LIST_X + 32;
    if (folder) {
        drawFolder(picture, x, y + 11, focused ? CARAFE_COLOR_ACCENT : CARAFE_COLOR_HINT);
        x += 28;
    }
    char shown[TEXT_SIZE];
    fitText(value, 16, CarafeBold, 250, shown, sizeof(shown));
    int value_w = carafeTextWidth(shown, 16, CarafeBold);
    int right = LIST_X + LIST_W - 16 - value_w;
    int tag_w = tag != NULL ? carafeTextWidth(tag, 11, CarafeBold) + 16 + 10 : 0;
    char label[TEXT_SIZE];
    fitText(name, 16, CarafeRegular, right - tag_w - x - 12, label, sizeof(label));
    CarafeTextStyle name_style = { 16, CarafeRegular, ROW_H, ink, false };
    carafeDrawLine(picture, label, &name_style, x, y, 0);
    if (tag != NULL) {
        drawTag(picture, right - 10, y, tag, focused, strong_tag);
    }
    CarafeTextStyle val = { 16, CarafeBold, ROW_H, focused ? CARAFE_COLOR_PRIMARY_INK : dim ? CARAFE_COLOR_DIM
                                                                                         : CARAFE_COLOR_INK,
        false };
    carafeDrawLine(picture, shown, &val, right, y, 0);
    addHit(HitRegRow, index, LIST_X, y, LIST_W, ROW_H);
}

static void drawRegChips(CarafeImage* picture) {
    const RegStrings* r = reg();
    int x = LIST_X;
    for (int v = regHasRows() ? 0 : 1; v < 2; v++) {
        char name[TEXT_SIZE];
        if (v == 0) {
            snprintf(name, sizeof(name), r->from_app, carafeRegistryRowCount());
        } else {
            snprintf(name, sizeof(name), "%s", r->whole);
        }
        int w = carafeTextWidth(name, 14, CarafeBold) + 24;
        bool on = v == g_reg_view;
        carafeDrawRound(picture, x, 104, w, 32, 16, on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_WHITE, on ? 255 : 18);
        CarafeTextStyle style = { 14, CarafeBold, 32, on ? 0xFF22121Au : CARAFE_COLOR_HINT, true };
        carafeDrawLine(picture, name, &style, x, 104, w);
        addHit(HitRegChip, v, x, 104, w, 32);
        x += w + 6;
    }
}

static void regPathParts(char* head, size_t head_size, char* tail, size_t tail_size) {
    head[0] = '\0';
    if (g_reg_hive < 0) {
        snprintf(tail, tail_size, "%s", reg()->computer);
        return;
    }
    const char* hive = carafeRegistryHiveName(g_reg_hive);
    if (g_reg_path[0] == '\0') {
        snprintf(tail, tail_size, "%s", hive);
        return;
    }
    const char* slash = strrchr(g_reg_path, '\\');
    snprintf(tail, tail_size, "%s", slash != NULL ? slash + 1 : g_reg_path);
    size_t used = (size_t)snprintf(head, head_size, "%s \\ ", hive);
    for (const char* p = g_reg_path; slash != NULL && p < slash && used + 4 < head_size; p++) {
        if (*p == '\\') {
            used += (size_t)snprintf(head + used, head_size - used, " \\ ");
        } else {
            head[used++] = *p;
            head[used] = '\0';
        }
    }
    if (slash != NULL && used + 4 < head_size) {
        snprintf(head + used, head_size - used, " \\ ");
    }
}

static void drawRegPath(CarafeImage* picture) {
    carafeDrawRound(picture, LIST_X, 150, LIST_W, 34, 10, CARAFE_COLOR_PANEL, 128);
    drawFolder(picture, LIST_X + 14, 157, CARAFE_COLOR_HINT);
    char head[REG_LINE];
    char tail[REG_LINE];
    regPathParts(head, sizeof(head), tail, sizeof(tail));
    int x = LIST_X + 44;
    int room = LIST_W - 60;
    char shown_tail[REG_LINE];
    fitText(tail, 14, CarafeBold, room / 2, shown_tail, sizeof(shown_tail));
    int tail_w = carafeTextWidth(shown_tail, 14, CarafeBold);
    if (head[0] != '\0') {
        char shown[REG_LINE + 8];
        if (carafeTextWidth(head, 14, CarafeRegular) > room - tail_w) {
            const char* cut = head;
            while (*cut != '\0' && carafeTextWidth(cut, 14, CarafeRegular) + carafeTextWidth("… ", 14, CarafeRegular)
                    > room - tail_w) {
                cut++;
                while ((*cut & 0xC0) == 0x80) {
                    cut++;
                }
            }
            snprintf(shown, sizeof(shown), "… %s", cut);
        } else {
            snprintf(shown, sizeof(shown), "%s", head);
        }
        CarafeTextStyle style = { 14, CarafeRegular, 34, CARAFE_COLOR_HINT, false };
        carafeDrawLine(picture, shown, &style, x, 150, 0);
        x += carafeTextWidth(shown, 14, CarafeRegular);
    }
    CarafeTextStyle bold = { 14, CarafeBold, 34, CARAFE_COLOR_INK, false };
    carafeDrawLine(picture, shown_tail, &bold, x, 150, 0);
}

static void regShortPath(char* out, size_t size, const char* leaf) {
    snprintf(out, size, "%s%s%s%s%s", g_reg_hive == CARAFE_REG_HIVE_MACHINE ? "HKLM" : "HKCU",
        g_reg_path[0] != '\0' ? "\\" : "", g_reg_path, leaf != NULL ? "\\" : "", leaf != NULL ? leaf : "");
}

static int drawModeBox(CarafeImage* picture, const char* mode, uint32_t color, int bottom) {
    int x = CARD_X + 30;
    int width = CARD_W - 60;
    CarafeTextStyle style = { 14, CarafeRegular, 20, color, false };
    int mode_h = carafeWrappedHeight(mode, &style, width - 28) + 16;
    carafeDrawRound(picture, x - 4, bottom - 26 - mode_h, width + 8, mode_h, 12, color, 30);
    carafeDrawWrapped(picture, mode, &style, x + 10, bottom - 26 - mode_h + 8, width - 28);
    return bottom - 26 - mode_h;
}

static void drawRegCard(CarafeImage* picture, int top) {
    const RegStrings* r = reg();
    int bottom = LIST_BOTTOM;
    carafeDrawRound(picture, CARD_X, top, CARD_W, bottom - top, 24, CARAFE_COLOR_PANEL, 128);
    carafeDrawRoundStroke(picture, CARD_X, top, CARD_W, bottom - top, 24, 1, CARAFE_COLOR_WHITE, 20);
    int x = CARD_X + 30;
    int width = CARD_W - 60;
    CarafeTextStyle title = { 24, CarafeHeavy, 29, CARAFE_COLOR_INK, false };
    CarafeTextStyle body = { 16, CarafeRegular, 24, CARAFE_COLOR_TEXT, false };
    CarafeTextStyle small = { 13, CarafeRegular, 19, CARAFE_COLOR_LABEL, false };
    int focus = *regFocus();
    char line[REG_LINE];
    if (g_reg_view == 0) {
        if (carafeRegistryRowCount() == 0) {
            return;
        }
        int y = carafeDrawWrapped(picture, carafeRegistryRowName(focus), &title, x, top + 26, width) + 8;
        if (carafeRegistryRowHint(focus)[0] != '\0') {
            y = carafeDrawWrapped(picture, carafeRegistryRowHint(focus), &body, x, y, width);
        }
        y += 10;
        int current = carafeRegistryRowCurrent(focus);
        int mode_top = bottom - 26 - 60;
        for (int i = 0; i < carafeRegistryRowOptionCount(focus) && y + 34 <= mode_top - 50; i++) {
            bool on = i == current;
            if (on) {
                carafeDrawRound(picture, x - 4, y, width + 8, 32, 10, CARAFE_COLOR_FOCUS, 41);
            }
            carafeDrawRing(picture, x + 10, y + 16, 8, on ? 5 : 2, on ? CARAFE_COLOR_FOCUS : CARAFE_COLOR_DIM, 255);
            CarafeTextStyle option = { 15, on ? CarafeBold : CarafeRegular, 32, on ? CARAFE_COLOR_INK : CARAFE_COLOR_TEXT,
                false };
            carafeDrawLine(picture, carafeRegistryRowOptionLabel(focus, i), &option, x + 28, y, 0);
            y += 34;
        }
        char where[CARAFE_REG_PATH];
        carafeRegistryRowWhere(focus, where, sizeof(where));
        snprintf(line, sizeof(line), "%s %s", r->writes, where);
        carafeDrawWrapped(picture, line, &small, x, y + 8, width);
        drawModeBox(picture, r->mode_row, CARAFE_COLOR_RESTART, bottom);
        return;
    }
    if (g_reg_hive < 0) {
        int hive = rootHive(focus);
        int y = carafeDrawWrapped(picture, carafeRegistryHiveName(hive), &title, x, top + 26, width) + 8;
        carafeDrawWrapped(picture, r->hive_hint[hive], &body, x, y, width);
        drawModeBox(picture, r->mode_key, CARAFE_COLOR_HINT, bottom);
        return;
    }
    char head[REG_LINE];
    char tail[REG_LINE];
    regPathParts(head, sizeof(head), tail, sizeof(tail));
    if (g_reg_count == 0) {
        int y = carafeDrawWrapped(picture, tail, &title, x, top + 26, width) + 8;
        carafeDrawWrapped(picture, g_reg_missing ? r->missing : r->empty, &body, x, y, width);
        if (!g_reg_missing) {
            drawModeBox(picture, r->mode_empty, CARAFE_COLOR_HINT, bottom);
        }
        return;
    }
    const CarafeRegEntry* entry = &g_reg_entries[focus < g_reg_count ? focus : g_reg_count - 1];
    const char* name = entry->name[0] != '\0' ? entry->name : "@";
    int y = carafeDrawWrapped(picture, name, &title, x, top + 26, width) + 8;
    if (entry->key) {
        regShortPath(line, sizeof(line), entry->name);
        char ref[REG_LINE + 64];
        snprintf(ref, sizeof(ref), "%s %s", r->path, line);
        carafeDrawWrapped(picture, ref, &small, x, y, width);
        drawModeBox(picture, entry->state == CarafeRegDeleted ? r->mode_deleted : r->mode_key, CARAFE_COLOR_HINT,
            bottom);
        return;
    }
    char type_name[64];
    y = carafeDrawWrapped(picture, regTypeName(entry->type, type_name, sizeof(type_name)), &body, x, y, width) + 8;
    char value[TEXT_SIZE * 2];
    carafeRegistryFormat(entry->type, entry->data, entry->size, value, sizeof(value));
    CarafeTextStyle big = { 22, CarafeHeavy, 28, CARAFE_COLOR_INK, false };
    int box_h = carafeWrappedHeight(value[0] != '\0' ? value : " ", &big, width - 28);
    if (box_h > 140) {
        box_h = 140;
    }
    bool hex = regNumber(entry->type) && entry->size >= 4;
    int box_total = box_h + 20 + (hex ? 22 : 0);
    carafeDrawRound(picture, x - 4, y, width + 8, box_total, 12, CARAFE_COLOR_WHITE, 15);
    carafeDrawWrapped(picture, value[0] != '\0' ? value : " ", &big, x + 10, y + 10, width - 28);
    if (hex) {
        unsigned long long number = 0;
        for (uint32_t i = 0; i < entry->size && i < 8; i++) {
            number |= (unsigned long long)entry->data[i] << (8 * i);
        }
        char hex_text[32];
        snprintf(hex_text, sizeof(hex_text), entry->size == 4 ? "0x%08llX" : "0x%016llX", number);
        carafeDrawText(picture, hex_text, 14, CarafeRegular, x + 10, y + 10 + box_h + 2, CARAFE_COLOR_LABEL);
    }
    y += box_total + 8;
    if (entry->state == CarafeRegChanged) {
        char was[TEXT_SIZE];
        char shown[TEXT_SIZE * 2];
        carafeRegistryFormat(entry->live_type, entry->live_data, entry->live_size, was, sizeof(was));
        snprintf(shown, sizeof(shown), r->was, was);
        y = carafeDrawWrapped(picture, shown, &small, x, y, width) + 4;
    }
    regShortPath(line, sizeof(line), NULL);
    char ref[REG_LINE + 64];
    snprintf(ref, sizeof(ref), "%s %s", r->path, line);
    carafeDrawWrapped(picture, ref, &small, x, y, width);
    const char* mode = r->mode_value;
    uint32_t color = CARAFE_COLOR_RESTART;
    if (g_reg_armed) {
        mode = r->delete_again;
    } else if (entry->state == CarafeRegDeleted) {
        mode = r->mode_deleted;
    } else if (entry->state == CarafeRegAdded) {
        mode = r->mode_added;
    } else if (!regEditable(entry)) {
        mode = r->mode_readonly;
        color = CARAFE_COLOR_HINT;
    }
    drawModeBox(picture, mode, color, bottom);
}

static void drawRegistry(CarafeImage* picture) {
    const RegStrings* r = reg();
    drawRegChips(picture);
    int top = g_reg_view == 0 ? LIST_TOP_PAGES : REG_LIST_TOP;
    if (g_reg_view == 1) {
        drawRegPath(picture);
    }
    int count = regListCount();
    int* focus = regFocus();
    int* scroll = regScroll();
    if (*focus >= count) {
        *focus = count > 0 ? count - 1 : 0;
    }
    int visible = (LIST_BOTTOM - top) / (ROW_H + ROW_GAP);
    if (*focus < *scroll) {
        *scroll = *focus;
    }
    if (*focus >= *scroll + visible) {
        *scroll = *focus - visible + 1;
    }
    int y = top;
    for (int i = *scroll; i < count && i < *scroll + visible; i++) {
        bool focused = i == *focus;
        if (g_reg_view == 0) {
            int current = carafeRegistryRowCurrent(i);
            const char* value = current >= 0 ? carafeRegistryRowOptionLabel(i, current) : r->own_value;
            drawRegRow(picture, i, y + 3, focused, carafeRegistryRowName(i), value, NULL, false,
                carafeRegistryRowChanged(i), false, false);
        } else if (g_reg_hive < 0) {
            drawRegRow(picture, i, y + 3, focused, carafeRegistryHiveName(rootHive(i)), "›", NULL, false, false, false,
                true);
        } else {
            const CarafeRegEntry* entry = &g_reg_entries[i];
            const char* name = entry->name[0] != '\0' ? entry->name : "@";
            bool deleted = entry->state == CarafeRegDeleted;
            if (entry->key) {
                drawRegRow(picture, i, y + 3, focused, name, "›", deleted ? r->tags[1] : NULL, true, deleted, deleted,
                    true);
            } else {
                char value[TEXT_SIZE];
                carafeRegistryFormat(entry->type, entry->data, entry->size, value, sizeof(value));
                const char* tag = entry->state == CarafeRegAdded ? r->tags[0]
                    : deleted                                    ? r->tags[1]
                                                                 : regTypeTag(entry->type);
                bool strong = entry->state == CarafeRegAdded || deleted;
                drawRegRow(picture, i, y + 3, focused, name, value, tag, strong, entry->state != CarafeRegLive, deleted,
                    false);
            }
        }
        y += ROW_H + ROW_GAP;
    }
    if (*scroll + visible < count) {
        carafeDrawText(picture, "…", 18, CarafeBold, LIST_X + LIST_W / 2, LIST_BOTTOM - 8, CARAFE_COLOR_LABEL);
    }
    drawRegCard(picture, LIST_TOP_PAGES);
}

static int drawEditField(CarafeImage* picture, int x, int y, int w, const char* label, const char* text, bool on,
    bool caret, const char* prefix, int field) {
    CarafeTextStyle caption = { 13, CarafeBold, 18, CARAFE_COLOR_LABEL, false };
    carafeDrawLine(picture, label, &caption, x, y, 0);
    y += 22;
    carafeDrawRound(picture, x, y, w, 52, 14, CARAFE_COLOR_GAP, 255);
    carafeDrawRoundStroke(picture, x, y, w, 52, 14, 2, on ? CARAFE_COLOR_FOCUS : 0xFF3A1A27u, 255);
    int tx = x + 16;
    if (prefix != NULL) {
        carafeDrawText(picture, prefix, 22, CarafeRegular, tx, y + 11, CARAFE_COLOR_LABEL);
        tx += carafeTextWidth(prefix, 22, CarafeRegular) + 4;
    }
    char shown[CARAFE_REG_NAME + 8];
    fitText(text, 22, CarafeBold, w - (tx - x) - 24, shown, sizeof(shown));
    carafeDrawText(picture, shown, 22, CarafeBold, tx, y + 11, CARAFE_COLOR_INK);
    if (caret) {
        carafeDrawRound(picture, tx + carafeTextWidth(shown, 22, CarafeBold) + 2, y + 12, 2, 28, 0, CARAFE_COLOR_FOCUS,
            255);
    }
    addHit(HitRegField, field, x, y, w, 52);
    return y + 52 + 12;
}

static int drawSegment(CarafeImage* picture, int x, int y, int w, const char* left, const char* right, bool right_on,
    bool focused, int field) {
    carafeDrawRound(picture, x, y, w, 44, 14, CARAFE_COLOR_GAP, 255);
    if (focused) {
        carafeDrawRoundStroke(picture, x, y, w, 44, 14, 2, CARAFE_COLOR_FOCUS, 255);
    }
    int half = (w - 8) / 2;
    for (int i = 0; i < 2; i++) {
        bool on = (i == 1) == right_on;
        int bx = x + 4 + i * half;
        if (on) {
            carafeDrawRound(picture, bx, y + 4, half, 36, 10, CARAFE_COLOR_FOCUS, 255);
        }
        CarafeTextStyle style = { 15, CarafeBold, 36, on ? 0xFF22121Au : CARAFE_COLOR_HINT, true };
        carafeDrawLine(picture, i == 0 ? left : right, &style, bx, y + 4, half);
    }
    addHit(HitRegField, field, x, y, w, 44);
    return y + 44 + 12;
}

static void drawEditor(CarafeImage* picture) {
    const RegStrings* r = reg();
    const Strings* s = text();
    carafeDrawRound(picture, 0, 0, picture->width, picture->height, 0, 0xFF0E0408u, 184);
    g_hit_count = 0;
    const char* const* keys;
    int columns;
    int count = editKeys(&keys, &columns);
    int rows = (count + columns - 1) / columns;
    const int key_h = 44;
    const int w = REG_EDIT_W;
    const int x = (picture->width - w) / 2;
    bool number = regNumber(g_edit_type);
    bool value_on = !g_edit_new || g_edit_field == 2;
    bool base_shown = number && value_on;
    int h = 30 + 34 + 26 + (g_edit_new ? 74 + 78 : 0) + 74 + (base_shown ? 56 : 0) + rows * (key_h + 6) + 16 + 56 + 30
        + (g_edit_error != NULL ? 24 : 0);
    if (h > picture->height - 16) {
        h = picture->height - 16;
    }
    const int y = (picture->height - h) / 2;
    carafeDrawRound(picture, x, y, w, h, 24, 0xFF1C0B13u, 255);
    carafeDrawRoundStroke(picture, x, y, w, h, 24, 1, CARAFE_COLOR_WHITE, 26);
    int inner = x + 36;
    int inner_w = w - 72;
    CarafeTextStyle title = { 26, CarafeHeavy, 32, CARAFE_COLOR_INK, false };
    char line[REG_LINE + 128];
    char where[REG_LINE];
    regShortPath(where, sizeof(where), NULL);
    carafeDrawLine(picture, g_edit_new ? r->new_value : (g_edit_name[0] != '\0' ? g_edit_name : "@"), &title, inner,
        y + 24, 0);
    char type_name[64];
    if (g_edit_new) {
        snprintf(line, sizeof(line), r->in_key, where);
    } else {
        snprintf(line, sizeof(line), "%s · %s", where, regTypeName(g_edit_type, type_name, sizeof(type_name)));
    }
    char shown[REG_LINE + 128];
    fitText(line, 14, CarafeRegular, inner_w, shown, sizeof(shown));
    carafeDrawText(picture, shown, 14, CarafeRegular, inner, y + 58, CARAFE_COLOR_LABEL);
    int ty = y + 90;
    if (g_edit_new) {
        ty = drawEditField(picture, inner, ty, inner_w, r->name, g_edit_name, g_edit_field == 0, g_edit_field == 0,
            NULL, 0);
        CarafeTextStyle caption = { 13, CarafeBold, 18, CARAFE_COLOR_LABEL, false };
        carafeDrawLine(picture, r->type, &caption, inner, ty, 0);
        ty = drawSegment(picture, inner, ty + 22, inner_w, r->number, r->string, g_edit_type == CARAFE_REG_SZ,
            g_edit_field == 1, 1);
    }
    ty = drawEditField(picture, inner, ty, inner_w, r->value, g_edit_text, value_on, value_on,
        number && g_edit_hex ? "0x" : NULL, 2);
    if (base_shown) {
        unsigned long long value = g_edit_text[0] != '\0' ? strtoull(g_edit_text, NULL, g_edit_hex ? 16 : 10) : 0;
        char dec[64];
        char hex[64];
        snprintf(dec, sizeof(dec), "%s · %llu", r->decimal, value);
        snprintf(hex, sizeof(hex), "%s · 0x%llX", r->hex, value);
        ty = drawSegment(picture, inner, ty - 4, inner_w, dec, hex, g_edit_hex, false, 3);
    }
    int key_w = (inner_w - (columns - 1) * 6) / columns;
    bool grid_on = !editOnType();
    for (int i = 0; i < count; i++) {
        int kx = inner + (i % columns) * (key_w + 6);
        int ky = ty + (i / columns) * (key_h + 6);
        const char* label = keys[i];
        char upper[4];
        if (strcmp(label, "SPACE") == 0) {
            label = r->space;
        } else if (strcmp(label, "CLR") == 0) {
            label = s->clear;
        } else if (g_edit_shift && label[0] >= 'a' && label[0] <= 'z' && label[1] == '\0') {
            snprintf(upper, sizeof(upper), "%c", label[0] - 'a' + 'A');
            label = upper;
        }
        bool focus = grid_on && i == g_edit_key;
        bool lit = strcmp(keys[i], "Aa") == 0 && g_edit_shift;
        if (focus) {
            carafeDrawFocusRing(picture, kx, ky, key_w, key_h, 10);
        } else {
            carafeDrawRound(picture, kx, ky, key_w, key_h, 10, lit ? CARAFE_COLOR_ACCENT : CARAFE_COLOR_WHITE,
                lit ? 255 : 20);
        }
        int size = 16;
        while (size > 10 && carafeTextWidth(label, size, CarafeBold) > key_w - 6) {
            size--;
        }
        CarafeTextStyle style = { size, CarafeBold, key_h, focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_VALUE, true };
        carafeDrawLine(picture, label, &style, kx, ky, key_w);
        addHit(HitRegKey, i, kx, ky, key_w, key_h);
    }
    ty += rows * (key_h + 6) + 10;
    if (g_edit_error != NULL) {
        carafeDrawText(picture, g_edit_error, 15, CarafeBold, inner, ty, CARAFE_COLOR_RESTART);
        ty += 24;
    }
    int bx = inner;
    const char* labels[2] = { s->done, s->cancel };
    const char* glyphs[2] = { "+", "B" };
    const ButtonId ids[2] = { ButtonEditDone, ButtonEditCancel };
    for (int i = 0; i < 2; i++) {
        int bw = 14 + 46 + carafeTextWidth(labels[i], 20, CarafeBold) + 30;
        if (i == 0) {
            carafeDrawRound(picture, bx, ty, bw, 52, 16, CARAFE_COLOR_INK, 255);
        } else {
            carafeDrawRound(picture, bx, ty, bw, 52, 16, CARAFE_COLOR_WHITE, 18);
            carafeDrawRoundStroke(picture, bx, ty, bw, 52, 16, 2, CARAFE_COLOR_WHITE, 36);
        }
        carafeDrawCircle(picture, bx + 14 + 17, ty + 26, 17, i == 0 ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_GAP, 255);
        CarafeTextStyle g = { 19, CarafeHeavy, 34, CARAFE_COLOR_INK, true };
        carafeDrawLine(picture, glyphs[i], &g, bx + 14, ty + 9, 34);
        CarafeTextStyle label = { 20, CarafeBold, 52, i == 0 ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_INK, false };
        carafeDrawLine(picture, labels[i], &label, bx + 60, ty, 0);
        addHit(HitButton, ids[i], bx, ty, bw, 52);
        bx += bw + 14;
    }
    int hx = bx + 10;
    int hy = ty + 14;
    hx = drawHint(picture, hx, hy, "A", r->press);
    hx = drawHint(picture, hx, hy, "X", r->erase);
    if (base_shown) {
        hx = drawHint(picture, hx, hy, "Y", r->base);
    }
    if (g_edit_new) {
        drawHint(picture, hx, hy, "L R", r->field);
    }
}

static void drawConfirm(CarafeImage* picture) {
    const Strings* s = text();
    carafeDrawRound(picture, 0, 0, picture->width, picture->height, 0, 0xFF0E0408u, 184);
    int pending[MAX_ITEMS];
    int count = 0;
    for (int i = 0; i < g_catalog->setting_count && count < 6; i++) {
        if (carafeSettingPending(&g_catalog->settings[i])) {
            pending[count++] = i;
        }
    }
    int registry = g_reg_available ? carafeRegistryPendingCount() : 0;
    if (registry > 6 - count) {
        registry = 6 - count;
    }
    const int w = 700;
    const int x = (picture->width - w) / 2;
    const int h = 250 + (count + registry) * 44 + 90;
    const int y = (picture->height - h) / 2;
    carafeDrawRound(picture, x, y, w, h, 24, 0xFF1C0B13u, 255);
    carafeDrawRoundStroke(picture, x, y, w, h, 24, 1, CARAFE_COLOR_WHITE, 26);
    CarafeTextStyle title = { 28, CarafeHeavy, 34, CARAFE_COLOR_INK, false };
    int ty = carafeDrawWrapped(picture, s->confirm_title, &title, x + 36, y + 30, w - 72) + 6;
    CarafeTextStyle body = { 17, CarafeRegular, 25, CARAFE_COLOR_TEXT, false };
    ty = carafeDrawWrapped(picture, s->confirm_body, &body, x + 36, ty, w - 72) + 10;
    for (int i = 0; i < count; i++) {
        const CarafeSetting* setting = &g_catalog->settings[pending[i]];
        const char* from = setting->start >= 0 ? pick(setting->values[setting->start].label) : setting->start_text;
        char line[TEXT_SIZE];
        snprintf(line, sizeof(line), "%s → %s", from, carafeValueLabel(setting));
        carafeDrawText(picture, pick(setting->name), 17, CarafeRegular, x + 36, ty + 10, CARAFE_COLOR_VALUE);
        int lw = carafeTextWidth(line, 17, CarafeBold);
        carafeDrawText(picture, line, 17, CarafeBold, x + w - 36 - lw, ty + 10, CARAFE_COLOR_INK);
        carafeDrawRound(picture, x + 36, ty + 43, w - 72, 1, 0, CARAFE_COLOR_WHITE, 26);
        ty += 44;
    }
    for (int i = 0; i < registry; i++) {
        char name[CARAFE_REG_NAME];
        char from[TEXT_SIZE];
        char to[TEXT_SIZE];
        if (!carafeRegistryPendingLine(i, name, sizeof(name), from, sizeof(from), to, sizeof(to))) {
            continue;
        }
        char change[TEXT_SIZE * 2 + 8];
        snprintf(change, sizeof(change), "%s → %s", from, to[0] != '\0' ? to : reg()->deleted);
        char line[TEXT_SIZE];
        fitText(change, 17, CarafeBold, (w - 72) / 2, line, sizeof(line));
        int lw = carafeTextWidth(line, 17, CarafeBold);
        char label[CARAFE_REG_NAME];
        fitText(name, 17, CarafeRegular, w - 72 - lw - 16, label, sizeof(label));
        carafeDrawText(picture, label, 17, CarafeRegular, x + 36, ty + 10, CARAFE_COLOR_VALUE);
        carafeDrawText(picture, line, 17, CarafeBold, x + w - 36 - lw, ty + 10, CARAFE_COLOR_INK);
        carafeDrawRound(picture, x + 36, ty + 43, w - 72, 1, 0, CARAFE_COLOR_WHITE, 26);
        ty += 44;
    }
    ty += 14;
    int box = ty;
    carafeDrawRound(picture, x + 36, box, w - 72, 64, 16, CARAFE_COLOR_WHITE, 13);
    if (g_trial) {
        carafeDrawRound(picture, x + 52, box + 18, 28, 28, 8, CARAFE_COLOR_FOCUS, 255);
        carafeDrawText(picture, "✓", 18, CarafeHeavy, x + 58, box + 18, 0xFF22121Au);
    } else {
        carafeDrawRoundStroke(picture, x + 52, box + 18, 28, 28, 8, 2, CARAFE_COLOR_DIM, 255);
    }
    carafeDrawText(picture, s->trial, 18, CarafeBold, x + 96, box + 8, CARAFE_COLOR_INK);
    carafeDrawText(picture, s->trial_hint, 14, CarafeRegular, x + 96, box + 34, CARAFE_COLOR_HINT);
    drawGlyph(picture, x + w - 36 - 24, box + 20, "X", CARAFE_COLOR_PRIMARY_INK, CARAFE_COLOR_INK);
    addHit(HitButton, ButtonTrial, x + 36, box, w - 72, 64);
    int by = box + 84;
    const char* labels[3] = { s->restart, s->later, s->undo };
    const ButtonId ids[3] = { ButtonConfirm, ButtonLater, ButtonUndo };
    int bx = x + 36;
    for (int i = 0; i < 3; i++) {
        bool focus = i == g_confirm_focus;
        int bw = (focus ? 14 + 46 : 30) + carafeTextWidth(labels[i], 20, CarafeBold) + 30;
        if (focus) {
            carafeDrawFocusRing(picture, bx, by, bw, 56, 16);
            carafeDrawCircle(picture, bx + 14 + 17, by + 28, 17, CARAFE_COLOR_PRIMARY_INK, 255);
            CarafeTextStyle g = { 19, CarafeHeavy, 34, CARAFE_COLOR_INK, true };
            carafeDrawLine(picture, "A", &g, bx + 14, by + 11, 34);
        } else {
            carafeDrawRound(picture, bx, by, bw, 56, 16, CARAFE_COLOR_WHITE, 18);
            carafeDrawRoundStroke(picture, bx, by, bw, 56, 16, 2, CARAFE_COLOR_WHITE, 36);
        }
        CarafeTextStyle label = { 20, CarafeBold, 56, focus ? CARAFE_COLOR_PRIMARY_INK : CARAFE_COLOR_INK, false };
        carafeDrawLine(picture, labels[i], &label, bx + (focus ? 14 + 46 : 30), by, 0);
        addHit(HitButton, ids[i], bx, by, bw, 56);
        bx += bw + 14;
    }
}

static void drawFooter(CarafeImage* picture) {
    const Strings* s = text();
    int x = LIST_X;
    int pending = pendingTotal();
    if (g_picker) {
        x = drawButtonAt(picture, x, "+", s->done, 1, ButtonDone);
        drawButtonAt(picture, x, NULL, s->clear, 0, ButtonClear);
    } else if (g_error_mode) {
        char label[TEXT_SIZE];
        if (pending > 0) {
            snprintf(label, sizeof(label), s->restart_count, pending);
        } else {
            snprintf(label, sizeof(label), "%s", s->restart);
        }
        x = drawButtonAt(picture, x, "+", label, pending > 0 ? 2 : 1, ButtonRestart);
        drawButtonAt(picture, x, NULL, s->to_error, 0, ButtonGame);
    } else {
        if (pending > 0) {
            char label[TEXT_SIZE];
            snprintf(label, sizeof(label), s->restart_count, pending);
            x = drawButtonAt(picture, x, "+", label, 2, ButtonRestart);
        }
        drawButtonAt(picture, x, NULL, s->to_game, 0, ButtonGame);
    }
    int hx = CARD_X;
    int hy = BUTTONS_Y + 18;
    if (g_reg) {
        const RegStrings* r = reg();
        const char* keys[6];
        const char* labels[6];
        int count = 0;
        if (g_reg_view == 0) {
            keys[count] = "A";
            labels[count++] = s->change;
            keys[count] = "Y";
            labels[count++] = s->reset;
            keys[count] = "L R";
            labels[count++] = r->whole;
            keys[count] = "B";
            labels[count++] = s->back;
        } else {
            keys[count] = "A";
            labels[count++] = r->open_change;
            if (g_reg_hive >= 0) {
                keys[count] = "X";
                labels[count++] = r->new_value;
                keys[count] = "Y";
                labels[count++] = r->undo;
                keys[count] = "−";
                labels[count++] = r->remove;
            }
            if (regHasRows()) {
                keys[count] = "L R";
                labels[count++] = r->rows;
            }
            keys[count] = "B";
            labels[count++] = g_reg_hive >= 0 ? r->up : s->back;
        }
        hy = BUTTONS_Y + 2;
        for (int i = 0; i < count; i++) {
            int glyph = carafeTextWidth(keys[i], 12, CarafeHeavy) + 12;
            glyph = glyph < 24 ? 24 : glyph;
            int need = glyph + 6 + carafeTextWidth(labels[i], 14, CarafeRegular);
            if (hx > CARD_X && hx + need > CARD_X + CARD_W) {
                hx = CARD_X;
                hy += 30;
            }
            hx = drawHint(picture, hx, hy, keys[i], labels[i]);
        }
        return;
    }
    if (g_picker) {
        hx = drawHint(picture, hx, hy, "A", g_picker_pad ? s->choose : s->toggle);
        hx = drawHint(picture, hx, hy, "Y", s->clear);
        drawHint(picture, hx, hy, "B", s->cancel);
        return;
    }
    hx = drawHint(picture, hx, hy, "A", s->change);
    hx = drawHint(picture, hx, hy, "Y", s->reset);
    hx = drawHint(picture, hx, hy, "L R", g_group >= 0 ? s->pages : s->sections);
    drawHint(picture, hx, hy, "B", g_group >= 0 ? s->back : g_error_mode ? s->to_error : s->to_game);
}

static void renderLocked(CarafeImage* picture) {
    if (g_catalog == NULL) {
        return;
    }
    g_hit_count = 0;
    carafeDrawBackground(picture, g_error_mode ? 255 : MENU_OPACITY);
    drawHeader(picture);
    if (g_picker) {
        drawPicker(picture);
    } else if (g_reg) {
        drawRegistry(picture);
    } else if (g_group < 0 && tabIsControls(g_tab)) {
        drawPad(picture);
    } else {
        drawList(picture);
    }
    drawFooter(picture);
    if (g_reg && g_edit) {
        drawEditor(picture);
    }
    if (g_confirm) {
        g_hit_count = 0;
        drawConfirm(picture);
    }
}

void carafeMenuRender(CarafeImage* picture) {
    pthread_mutex_lock(&g_lock);
    renderLocked(picture);
    pthread_mutex_unlock(&g_lock);
}

bool carafeMenuVisible(void) {
    return __atomic_load_n(&g_open, __ATOMIC_ACQUIRE) && !g_error_mode;
}

unsigned int carafeMenuGeneration(void) {
    return __atomic_load_n(&g_generation, __ATOMIC_RELAXED);
}

static bool ensurePicture(int width, int height) {
    if (g_picture.pixels != NULL && g_picture.width == width && g_picture.height == height) {
        return true;
    }
    free(g_picture.pixels);
    g_picture.pixels = malloc((size_t)width * height * 4);
    g_picture.width = width;
    g_picture.height = height;
    g_rendered = 0;
    return g_picture.pixels != NULL;
}

static bool prepareLocked(int width, int height, unsigned int* generation) {
    if (!g_open || width <= 0 || height <= 0) {
        return false;
    }
    pthread_mutex_lock(&g_picture_lock);
    bool ok = ensurePicture(CARAFE_SCREEN_WIDTH, CARAFE_SCREEN_HEIGHT);
    if (ok) {
        unsigned int current = carafeMenuGeneration();
        if (g_rendered != current) {
            renderLocked(&g_picture);
            g_rendered = current;
        }
        *generation = g_rendered | 0x80000000u;
    }
    pthread_mutex_unlock(&g_picture_lock);
    return ok;
}

static void scaleMenuPicture(const CarafeImage* src, int dst_w, int dst_h, void* pixels, int stride, int rgba) {
    int stack_map[2048 * 3];
    int* map = dst_w <= 2048 ? stack_map : malloc((size_t)dst_w * 3 * sizeof(int));
    if (map == NULL) {
        return;
    }
    int* ix0 = map;
    int* ix1 = map + dst_w;
    int* fx = map + dst_w * 2;
    int src_w = src->width;
    int src_h = src->height;
    int max_u = (src_w - 1) * 256;
    int max_v = (src_h - 1) * 256;
    for (int x = 0; x < dst_w; x++) {
        int64_t center = ((int64_t)(2 * x + 1) * src_w * 256) / (2 * dst_w) - 128;
        if (center < 0) {
            center = 0;
        } else if (center > max_u) {
            center = max_u;
        }
        int x0 = (int)(center >> 8);
        ix0[x] = x0;
        ix1[x] = x0 + 1 < src_w ? x0 + 1 : x0;
        fx[x] = (int)(center & 255);
    }
    for (int y = 0; y < dst_h; y++) {
        int64_t center = ((int64_t)(2 * y + 1) * src_h * 256) / (2 * dst_h) - 128;
        if (center < 0) {
            center = 0;
        } else if (center > max_v) {
            center = max_v;
        }
        int y0 = (int)(center >> 8);
        int y1 = y0 + 1 < src_h ? y0 + 1 : y0;
        int fy = (int)(center & 255);
        int wy0 = 256 - fy;
        const uint32_t* row0 = src->pixels + (size_t)y0 * src_w;
        const uint32_t* row1 = src->pixels + (size_t)y1 * src_w;
        uint32_t* out = (uint32_t*)((uint8_t*)pixels + (size_t)y * stride);
        for (int x = 0; x < dst_w; x++) {
            uint32_t p00 = row0[ix0[x]];
            uint32_t p10 = row0[ix1[x]];
            uint32_t p01 = row1[ix0[x]];
            uint32_t p11 = row1[ix1[x]];
            int fxx = fx[x];
            int wx0 = 256 - fxx;
            int w00 = wx0 * wy0;
            int w10 = fxx * wy0;
            int w01 = wx0 * fy;
            int w11 = fxx * fy;
            uint32_t c0 = (uint32_t)(((p00 & 0xFF) * w00 + (p10 & 0xFF) * w10 + (p01 & 0xFF) * w01
                                         + (p11 & 0xFF) * w11 + 32768)
                >> 16);
            uint32_t g = (uint32_t)((((p00 >> 8) & 0xFF) * w00 + ((p10 >> 8) & 0xFF) * w10
                                        + ((p01 >> 8) & 0xFF) * w01 + ((p11 >> 8) & 0xFF) * w11 + 32768)
                >> 16);
            uint32_t c2 = (uint32_t)((((p00 >> 16) & 0xFF) * w00 + ((p10 >> 16) & 0xFF) * w10
                                         + ((p01 >> 16) & 0xFF) * w01 + ((p11 >> 16) & 0xFF) * w11 + 32768)
                >> 16);
            uint32_t a = (uint32_t)((((p00 >> 24) & 0xFF) * w00 + ((p10 >> 24) & 0xFF) * w10
                                        + ((p01 >> 24) & 0xFF) * w01 + ((p11 >> 24) & 0xFF) * w11 + 32768)
                >> 16);
            out[x] = rgba ? (a << 24) | (c0 << 16) | (g << 8) | c2 : (a << 24) | (c2 << 16) | (g << 8) | c0;
        }
    }
    if (map != stack_map) {
        free(map);
    }
}

unsigned int carafeMenuCopy(int screen_width, int screen_height, void* pixels, int stride, int rgba) {
    if (pixels == NULL || screen_width <= 0 || screen_height <= 0 || stride < screen_width * 4) {
        return 0;
    }
    unsigned int generation = 0;
    pthread_mutex_lock(&g_lock);
    bool ok = prepareLocked(screen_width, screen_height, &generation);
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        return 0;
    }
    pthread_mutex_lock(&g_picture_lock);
    if (g_picture.pixels == NULL || g_picture.width <= 0 || g_picture.height <= 0) {
        pthread_mutex_unlock(&g_picture_lock);
        return 0;
    }
    if (screen_width == g_picture.width && screen_height == g_picture.height) {
        for (int y = 0; y < g_picture.height; y++) {
            uint32_t* out = (uint32_t*)((uint8_t*)pixels + (size_t)y * stride);
            const uint32_t* in = g_picture.pixels + (size_t)y * g_picture.width;
            if (!rgba) {
                memcpy(out, in, (size_t)g_picture.width * 4);
            } else {
                for (int x = 0; x < g_picture.width; x++) {
                    uint32_t p = in[x];
                    out[x] = (p & 0xFF00FF00u) | (p >> 16 & 0xFF) | (p & 0xFF) << 16;
                }
            }
        }
    } else {
        scaleMenuPicture(&g_picture, screen_width, screen_height, pixels, stride, rgba);
    }
    pthread_mutex_unlock(&g_picture_lock);
    return generation;
}

int wine_nx_osk_translucent(void) {
    return carafeMenuVisible();
}

void wine_nx_menu_poll(u64 held) {
    bool chord = (held & CHORD_BUTTONS) == CHORD_BUTTONS;
    bool was = g_chord_held != 0;
    g_chord_held = chord;
    if (!chord || was) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (g_open) {
        closeLocked();
    } else if (!openLocked(false)) {
        note("[CARAFE] settings menu: nothing to show");
    }
    pthread_mutex_unlock(&g_lock);
}

int carafeMenuHandleInput(unsigned int held, int stick_x, int stick_y, int touching, int touch_x, int touch_y,
    uint64_t now_ns) {
    if (!carafeMenuVisible()) {
        return 0;
    }
    unsigned int stick = 0;
    if (stick_y > 16000) {
        stick |= OSK_UP;
    } else if (stick_y < -16000) {
        stick |= OSK_DOWN;
    }
    if (stick_x > 16000) {
        stick |= OSK_RIGHT;
    } else if (stick_x < -16000) {
        stick |= OSK_LEFT;
    }
    CarafeMenuAction action = carafeMenuInput(held | stick, touching, touch_x, touch_y, now_ns);
    if (action == CarafeMenuBack) {
        pthread_mutex_lock(&g_lock);
        closeLocked();
        pthread_mutex_unlock(&g_lock);
    } else if (action == CarafeMenuRestart) {
        pthread_mutex_lock(&g_lock);
        bool trial = g_trial;
        closeLocked();
        pthread_mutex_unlock(&g_lock);
        note("[CARAFE] settings menu: restarting the game with the new settings%s", trial ? " (trial run)" : "");
        carafeRequestRestart(trial);
    }
    return 1;
}

bool carafeMenuOpenError(CarafeFailure failure) {
    pthread_mutex_lock(&g_lock);
    g_failure = failure;
    bool ok = openLocked(true);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void carafeMenuClose(void) {
    pthread_mutex_lock(&g_lock);
    closeLocked();
    pthread_mutex_unlock(&g_lock);
}
