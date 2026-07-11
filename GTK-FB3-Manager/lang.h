#pragma once

typedef enum {
    LANG_RU,
    LANG_EN
} Language;

void lang_set(Language lang);
Language lang_get(void);

const char *tr(const char *ru, const char *en);
