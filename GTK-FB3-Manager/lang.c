#include "lang.h"

static Language current = LANG_RU;

void lang_set(Language lang)
{
    current = lang;
}

Language lang_get(void)
{
    return current;
}

const char *tr(const char *ru, const char *en)
{
    return current == LANG_RU ? ru : en;
}
