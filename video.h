#ifndef __VIDEO_H__
#define __VIDEO_H__

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

extern char buffer1[];

int setupGraphics(int width, int height);
void read_png_file(char *filename);
void process_png_file();
void draw(void *data, GLuint width, GLuint height, GLuint pitch);
int closeWindow();

#endif
