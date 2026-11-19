// builtin.c - start the bundled apps built into the loader (builtin.h). Each app's main() is
// renamed by the build (esp32p4/main/CMakeLists.txt) and keeps its settings in /pml/<folder>.
#include <stdbool.h>
#include "plat.h"
#include "builtin.h"

#if PML_BUILTIN_APPS
#include "audio.h"
#include "pmlapp.h"

int builtin_image_main(void);
int builtin_music_main(void);
int builtin_calc_main(void);
void image_editor_release(void);

bool builtin_run(int which)
{
    int r;
    switch (which) {
    case BUILTIN_IMAGE:
        r = pmlapp_run("Image Editor", "/pml/ImageEditor", builtin_image_main);
        image_editor_release();
        break;
    case BUILTIN_MUSIC:
        r = pmlapp_run("Music Player", "/pml/MusicPlayer", builtin_music_main);
        audio_stop(); // in case it left playing
        break;
    case BUILTIN_CALC:
        r = pmlapp_run("Calculator", "/pml/Calculator", builtin_calc_main);
        break;
    default:
        return false;
    }
    return r == 0;
}
#endif
