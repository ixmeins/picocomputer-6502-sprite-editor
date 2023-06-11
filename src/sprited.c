/**
 * A simple sprite or character editor for the Picocomputer 6502 by @rumbledthumps (see youtube series).
 * 
 * Ingmar Meins (wtf) 2023
 * Various borrowed bit acknowledged etc blah blah.
 * 
 * As of 11 June 2023 there is a video rendering bug upstream which prevents pixel 0 from being rendered in each line.
*/
#include <rp6502.h>
#include <stdio.h>
#include "font8x8.h" // covers from ASCII 32 to 95 inclusive.

#define WIDTH 320
#define HEIGHT 240 // 180 or 240
#define LB 1 // Left border which should be zero if it were not for the rendering bug
#define RB 319
#define TB 0
#define BB 239

// The following assumes the original 16 colour ANSI palette. Now doubt we will
// be able to select palettes in the future.
#define BGCOL 0 // Used as background colour
#define FGCOL 7 // Used for things like the screen grids and borders

/**
 * vmode(mode)
 * 
 * Sets the video mode to 0 (text) 1 (320x180) or 2 (320x240) with a pixbus write using the xreg command 
*/
static void vmode(uint16_t data)
{
    xreg(data, 0, 1);
}

/**
 * gcls() 
 * 
 * Clear the graphics screen memory
*/
static void gcls(uint8_t c) {
    unsigned i = 0;

    c += c << 4; // fill the whole byte ie 2 pixels with colour.

    // Partially unrolled loop is FAST

    RIA_ADDR0 = 0; // Start address
    RIA_STEP0 = 1; // amount to auto step on each write to RIA_RW0 register

    for (i = 0x980; --i;) // 0x1300 is 38912d = 320x240 # 4bpp ie 2 pixels per byte
    {
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
        RIA_RW0 = c;
    }

}
/**
 * wait()
 * 
 * wait for any keypress before returning
*/
static void wait()
{
    uint8_t discard;
    discard = RIA_RX;
    while (RIA_RX_READY)
        discard = RIA_RX;
    while (!(RIA_RX_READY))
        ;
    discard = RIA_RX;
}

/**
 * setxyc(x,y,c)
 * 
 * Set a pixel at the specified coordinates using the colour c (0-15).
*/
void setxyc(uint16_t x, uint8_t y, int8_t c) {
    // c = c & 15; // Only keep colours 0-15 or assume the user has a brain and save the cpu cycles
    
    RIA_ADDR0 = (y * 160) + (x >> 1); // Start address
    RIA_STEP0 = 0;  // We don't actually care about the vram address in RIA_ADDR0 incrementing

    if ((x & 1) == 0)
        RIA_RW0 = (RIA_RW0 & 0xf0) | c; // right pixel in vram byte
    else
        RIA_RW0 = (RIA_RW0 & 0x0f) | (c << 4); // left pixel in vram byte
    
}

/**
 * fastline(x,y,x1,y1,c) 
 * Draws a (straight h or v only at the moment) in the specified colour. 
*/
void fastline(uint16_t x0, uint8_t y0, uint16_t x1, uint8_t y1, uint8_t c) {
    uint8_t rightpix = 0; // 1 if right pixel else 0 for left pixel
    uint8_t pair = 0;

    // Make both nibbles in the colour the same for later.
    c += c << 4;

    if (x0 == x1) { // vertical line
        // A vertical line needs a vram start address in RIA_ADDR0 and a step size of 0 in
        // RIA_STEP0 so after each write it does not advance since we need to a read modify write op.
        // Also need to determine if it is an odd or even X so we know which half of the vram byte
        // to write for this pixel.
        RIA_ADDR0 = (y0 * 160) + (x0 >> 1); // address of vram pixel pair
        RIA_STEP0 = 0;
        
        // Determine if this is a left or right nibble pixel
        rightpix = x0 & 1;

        while (y0 < y1+1) {
            if (rightpix == 0) {
                //RIA_ADDR0 = (y0 * 160) + (x0 >> 1); // address of vram pixel pair
                RIA_RW0 = (RIA_RW0 & 0xf0) | (c & 0x0f);
            } else {
                //RIA_ADDR0 = (y0 * 160) + (x0 >> 1); // address of vram pixel pair
                RIA_RW0 = (RIA_RW0 & 0x0f) | (c & 0xf0);
            }

            RIA_ADDR0 += 160;
            y0++;
        }

    } else { // horizontal line
        RIA_ADDR0 = (y0 * 160) + (x0 >> 1); // address of vram pixel pair
        RIA_STEP0 = 0;

        while (x0 < (x1+1)) { // As x is changing we need to keep track of left/right nibbles each loop
            if ((x0 & 1) == 0) {
                RIA_RW0 = (RIA_RW0 & 0xf0) | (c & 0x0f);
            } else {
                RIA_RW0 = (RIA_RW0 & 0x0f) | (c & 0xf0);
            }

            if (x0 & 1)
                RIA_ADDR0 += 1;
            x0++;
        }
    
    }
}

/**
 * render8x8(uint8_t *chargen, x, y, scale, fg, bg)
 *
 * Render an 8x8 pixel bitmap from memory to the screen with the top left
 * at x,y in single colours for foreground and background. Ie for text stuff.
 * Scale the pixels as per scale (1,2,3 etc.)
 * To say render a H you would have this in the *chargen array
 * 0b01000010,
 * 0b01000010,
 * 0b01000010,
 * 0b01111110,
 * 0b01000010,
 * 0b01000010,
 * 0b01000010,
 * 0b00000000
 * 
 * First version in slow mode calling setxyc()
*/
void render8x8(uint8_t * chrgen, uint16_t x, uint8_t y, uint8_t scale, uint8_t fg, uint8_t bg) {
    #define CH 8
    #define CW 8

    uint8_t height = CH, width = CW;
    uint16_t xtemp = x;

    for (height=CH; height>0; height--) { // each line
        xtemp = x; // left most pixel each line
        for (width=CW; width>0; width--) { // each pixel across
            if ((chrgen[8-height] & (1<<width-1)) == 0) // bit is bg colour
                setxyc(xtemp++,y,bg);
            else
                setxyc(xtemp++,y,fg);
        }
        y++; // next line
    }
}

/**
 * drawLayout()
 * 
 * Draw the borders around the different screen areas, static text etc.
*/
void drawLayout() {
    gcls(BGCOL);

    // Outside border
    fastline(LB,TB,LB,BB,FGCOL); // vertical line at right
    fastline(RB,TB,RB,BB,FGCOL); // Vertical line at left

    fastline(LB,TB,RB,TB,FGCOL); // horizontal line at top
    fastline(LB,BB,RB,BB,FGCOL); // horizontal line at bottom

    render8x8(&console_font_8x8[40], 10, 10, 1, 1,7);
    render8x8(&console_font_8x8[48], 20, 10, 1, 2,7);
    render8x8(&console_font_8x8[56], 30, 10, 1, 3,7);
    render8x8(&console_font_8x8[64], 40, 10, 1, 4,7);
    render8x8(&console_font_8x8[72], 50, 10, 1, 5,7);
    render8x8(&console_font_8x8[80], 60, 10, 1, 6,7);
    render8x8(&console_font_8x8[88], 70, 10, 1, 8,7);
    render8x8(&console_font_8x8[96], 80, 10, 1, 9,7);
    render8x8(&console_font_8x8[104], 90, 10, 1, 10,7);
    render8x8(&console_font_8x8[112], 100, 10, 1, 11,7);
    render8x8(&console_font_8x8[120], 110, 10, 1, 12,7);
    render8x8(&console_font_8x8[128], 120, 10, 1, 13,7);
}

void main()
{
    uint16_t x = 0;
    uint8_t y = 0;

    #if (HEIGHT == 180)
    vmode(2);
#else
    vmode(1);
#endif

    drawLayout();

    wait();
}
