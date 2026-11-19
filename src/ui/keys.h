/* Клавиши пульта (docs/controls.md): коды evdev -> имена, короткие и долгие нажатия, удержание ↑/↓ своим таймером.
 * Если у верхнего слоя есть долгое действие для клавиши, короткое срабатывает при отпускании, долгое — через
 * 500 мс (OK — 800 мс); после долгого повторы этой клавиши игнорируются до отпускания. */
#ifndef PTV_KEYS_H
#define PTV_KEYS_H

enum {
    KEY_NONE = 0, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_OK, KEY_BACK, KEY_MENU, KEY_INFO, KEY_GUIDE,
    KEY_PLAYPAUSE, KEY_PLAY, KEY_PAUSE, KEY_STOP, KEY_REW, KEY_FF, KEY_CH_UP, KEY_CH_DOWN,
    KEY_RED, KEY_GREEN, KEY_YELLOW, KEY_BLUE, KEY_TEXT, KEY_LIST, KEY_FAV, KEY_SUBTITLE, KEY_AUDIO, KEY_HOME, KEY_SEARCH,
    KEY_D0 = 48   /* KEY_D0..KEY_D0+9 */
};
enum { EV_SHORT = 0, EV_LONG = 1, EV_REPEAT = 2 };

#define IS_DIGIT(k) ((k) >= KEY_D0 && (k) <= KEY_D0 + 9)

int keys_map(int evdev_code);
/* событие evdev (value: 1 нажатие, 2 повтор, 0 отпускание) */
void keys_raw(int code, int value);
/* таймеры долгого нажатия и удержания; вызывать каждый кадр */
void keys_tick(double now);
#endif
