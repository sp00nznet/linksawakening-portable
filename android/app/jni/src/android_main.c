/* Android entry shim.
 *
 * SDL's Java glue (SDLActivity) loads libmain.so and dlsym()s a symbol
 * named "SDL_main". rom_main.c — shared verbatim with every other port —
 * exposes a plain main(). Forward one to the other here so rom_main.c
 * stays platform-agnostic. */

extern int main(int argc, char *argv[]);

__attribute__((visibility("default")))
int SDL_main(int argc, char *argv[])
{
    return main(argc, argv);
}
