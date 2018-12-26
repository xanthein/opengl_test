#include "video.h"

int main(int argc, char *argv[]) {

	setupGraphics(1280, 720);
	read_png_file("./testpattern-hd-720.png");
	process_png_file();
    
	while (!closeWindow())
		draw(buffer1, 1280, 720, 1280*4);

    return 0;
}
