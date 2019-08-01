#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "lcd.h"
#include "cpu.h"
#include "interrupt.h"
#include "sdl.h"
#include "mem.h"


static int lcd_line;
static int lcd_ly_compare;

static QueueHandle_t lcdqueue;


/* LCD STAT */
static int ly_int;	/* LYC = LY coincidence interrupt enable */
static int mode2_oam_int;
static int mode1_vblank_int;
static int mode0_hblank_int;
static int ly_int_flag;
static int lcd_mode;

/* LCD Control */
static int lcd_enabled;
static int window_tilemap_select;
static int window_enabled;
static int tilemap_select;
static int bg_tiledata_select;
static int sprite_size;
static int sprites_enabled;
static int bg_enabled;
static int scroll_x, scroll_y;
static int window_x, window_y;

static byte bgpalette[] = {3, 2, 1, 0};
static byte sprpalette1[] = {0, 1, 2, 3};
static byte sprpalette2[] = {0, 1, 2, 3};

struct sprite {
	int y, x, tile, flags;
};

enum {
	PRIO  = 0x80,
	VFLIP = 0x40,
	HFLIP = 0x20,
	PNUM  = 0x10
};

unsigned char lcd_get_stat(void)
{

	return (ly_int)<<6 | lcd_mode;
}

void lcd_write_bg_palette(unsigned char n)
{
	bgpalette[0] = (n>>0)&3;
	bgpalette[1] = (n>>2)&3;
	bgpalette[2] = (n>>4)&3;
	bgpalette[3] = (n>>6)&3;
}

void lcd_write_spr_palette1(unsigned char n)
{
	sprpalette1[0] = 0;
	sprpalette1[1] = (n>>2)&3;
	sprpalette1[2] = (n>>4)&3;
	sprpalette1[3] = (n>>6)&3;
}

void lcd_write_spr_palette2(unsigned char n)
{
	sprpalette2[0] = 0;
	sprpalette2[1] = (n>>2)&3;
	sprpalette2[2] = (n>>4)&3;
	sprpalette2[3] = (n>>6)&3;
}

void lcd_write_scroll_x(unsigned char n)
{
	scroll_x = n;
}

void lcd_write_scroll_y(unsigned char n)
{
	scroll_y = n;
}

int lcd_get_line(void)
{
	return lcd_line;
}

void lcd_write_stat(unsigned char c)
{
	ly_int = !!(c&0x40);
}

void lcd_write_control(unsigned char c)
{
	bg_enabled            = !!(c & 0x01);
	sprites_enabled       = !!(c & 0x02);
	sprite_size           = !!(c & 0x04);
	tilemap_select        = !!(c & 0x08);
	bg_tiledata_select    = !!(c & 0x10);
	window_enabled        = !!(c & 0x20);
	window_tilemap_select = !!(c & 0x40);
	lcd_enabled           = !!(c & 0x80);
	
	if(!lcd_enabled)
	{
		xQueueReset(lcdqueue);
		sdl_clear_framebuffer(0x00);
		//sdl_frame();
	}
}

void lcd_set_ly_compare(unsigned char c)
{
	lcd_ly_compare = c;
}

void lcd_set_window_y(unsigned char n) {
	window_y = n;
}

void lcd_set_window_x(unsigned char n) {
	window_x = n;
}

static void swap(struct sprite *a, struct sprite *b)
{
	struct sprite c;

	 c = *a;
	*a = *b;
	*b =  c;
}

static void sort_sprites(struct sprite *s, int n)
{
	int swapped, i;

	do
	{
		swapped = 0;
		for(i = 0; i < n-1; i++)
		{
			if(s[i].x < s[i+1].x)
			{
				swap(&s[i], &s[i+1]);
				swapped = 1;
			}
		}
	}
	while(swapped);
}

//void drawColorIndexToFrameBuffer(int x, int y, byte idx, byte *b) {
//  int offset = x + y * 160;
//  b[offset >> 2] &= ~(0x3 << ((offset & 3) << 1));
//  b[offset >> 2] |= (idx << ((offset & 3) << 1));
//}

static void draw_bg_and_window(byte *b, int line)
{
	int x, offset;
	offset = line * 160;
	unsigned char* mem = mem_get_bytes();

	for(x = 0; x < 160; x++)
	{
		unsigned int map_select, map_offset, tile_num, tile_addr, xm, ym;
		unsigned char b1, b2, mask, colour;

		/* Convert LCD x,y into full 256*256 style internal coords */
		if(line >= window_y && window_enabled && line - window_y < 144)
		{
			xm = x;
			ym = line - window_y;
			map_select = window_tilemap_select;
		}
		else {
			if(!bg_enabled)
			{
				//b[line*640 + x] = 0;
				//drawColorIndexToFrameBuffer(x,line,0,b);
				//b[offset >> 2] &= ~(0x3 << ((offset & 3) << 1));
				b[offset] = 0;
				return;
			}
			xm = (x + scroll_x)&0xFF;
			ym = (line + scroll_y)&0xFF;
			map_select = tilemap_select;
		}

		/* Which pixel is this tile on? Find its offset. */
		/* (y/8)*32 calculates the offset of the row the y coordinate is on.
		 * As 256/32 is 8, divide by 8 to map one to the other, this is the row number.
		 * Then multiply the row number by the width of a row, 32, to find the offset.
		 * Finally, add x/(256/32) to find the offset within that row. 
		 */
		map_offset = (ym/8)*32 + xm/8;

		tile_num = mem[0x9800 + map_select*0x400 + map_offset];
		if(bg_tiledata_select)
			tile_addr = 0x8000 + tile_num*16;
		else
			tile_addr = 0x9000 + ((signed char)tile_num)*16;

		b1 = mem[tile_addr+(ym&7)*2];
		b2 = mem[tile_addr+(ym&7)*2+1];
		mask = 128>>(xm&7);
		colour = (!!(b2&mask)<<1) | !!(b1&mask);
		//b[line*640 + x] = colours[bgpalette[colour]];
		//drawColorIndexToFrameBuffer(x,line,bgpalette[colour],b);
		b[offset] = bgpalette[colour];
		//b[offset >> 2] &= ~(0x3 << ((offset & 3) << 1));
		//b[offset >> 2] |= (bgpalette[colour] << ((offset & 3) << 1));
		++offset;
	}
}

static void draw_sprites(byte *b, int line, int nsprites, struct sprite *s)
{
	int i;
	unsigned char* mem = mem_get_bytes();

	for(i = 0; i < nsprites; i++)
	{
		unsigned int b1, b2, tile_addr, sprite_line, x, offset;

		/* Sprite is offscreen */
		if(s[i].x < -7)
			continue;

		/* Which line of the sprite (0-7) are we rendering */
		sprite_line = s[i].flags & VFLIP ? (sprite_size ? 15 : 7)-(line - s[i].y) : line - s[i].y;

		/* Address of the tile data for this sprite line */
		tile_addr = 0x8000 + (s[i].tile*16) + sprite_line*2;

		/* The two bytes of data holding the palette entries */
		b1 = mem[tile_addr];
		b2 = mem[tile_addr+1];

		/* For each pixel in the line, draw it */
		offset = s[i].x + line * 160;
		for(x = 0; x < 8; x++, offset++)
		{
			unsigned char mask, colour;
			byte *pal;

			if((s[i].x + x) >= 160)
				continue;

			mask = s[i].flags & HFLIP ? 128>>(7-x) : 128>>x;
			colour = ((!!(b2&mask))<<1) | !!(b1&mask);
			if(colour == 0)
				continue;

			pal = (s[i].flags & PNUM) ? sprpalette2 : sprpalette1;
			
			/* Sprite is behind BG, only render over palette entry 0 */
			if(s[i].flags & PRIO)
			{
				//unsigned int temp = b[line*640+(x + s[i].x)];
				//byte temp = (b[offset >> 2] >> ((offset & 3) << 1)) & 3;
				if(b[offset] != bgpalette[0])
					continue;
			}
			//b[line*640+(x + s[i].x)] = colours[pal[colour]];
			//drawColorIndexToFrameBuffer(x + s[i].x,line,pal[colour],b);
			b[offset] = pal[colour];
			//b[offset >> 2] &= ~(0x3 << ((offset & 3) << 1));
			//b[offset >> 2] |= (pal[colour] << ((offset & 3) << 1));
		}
	}
}

static void render_line(void *arg)
{
	int line = 0;
	while(true) {
		if (!xQueueReceive(lcdqueue, &line, portMAX_DELAY))
			continue;

		if (!lcd_enabled || lcd_mode==1)
			continue;

	//    if (line == 144) {
	//      sdl_render_framebuffer();
	//      continue;
	//    }
		
		int i, c = 0;
		
		struct sprite s[10];
		byte *b = sdl_get_framebuffer();
		unsigned char* mem = mem_get_bytes();
		
		for(i = 0; i<40; i++)
		{
			int y, offs = i * 4;
		
			y = mem[0xFE00 + offs++] - 16;
			if(line < y || line >= y + 8+(sprite_size*8))
				continue;
		
			s[c].y     = y;
			s[c].x     = mem[0xFE00 + offs++]-8;
			s[c].tile  = mem[0xFE00 + offs++];
			s[c].flags = mem[0xFE00 + offs++];
			c++;
		
			if(c == 10)
				break;
		}
		
		if(c)
			sort_sprites(s, c);
		
		/* Draw the background layer */
		draw_bg_and_window(b, line);
		
		draw_sprites(b, line, c, s);

		if (line == 143) {
			sdl_render_framebuffer();
		}
	}
}

// TODO: proper lcd timing
void lcd_cycle()
{   
	int cycles = cpu_get_cycles();
	int this_frame, subframe_cycles;
	static int prev_line;
  
	this_frame = cycles % (70224/4);
	lcd_line = this_frame / (456/4);
	
	if(!lcd_enabled)
		return;
  
	if(this_frame < 204/4)
		lcd_mode = 2;
	else if(this_frame < 284/4)
		lcd_mode = 3;
	else if(this_frame < 456/4)
		lcd_mode = 0;
	if(lcd_line >= 144)
		lcd_mode = 1;
		
	if(lcd_line != prev_line && lcd_line < 144)
		xQueueSend(lcdqueue, &lcd_line, 0);
		//render_line(lcd_line);
  
	if(ly_int && lcd_line == lcd_ly_compare)
		interrupt(INTR_LCDSTAT);
  
	if(prev_line == 143 && lcd_line == 144)
	{
		//draw_stuff();
    //xQueueSend(lcdqueue, &lcd_line, 0);
		interrupt(INTR_VBLANK);
		//sdl_frame();
	}
	prev_line = lcd_line;
}

//void lcd_cycle()
//{
//  int cycles = cpu_get_cycles();
//  xQueueSend(lcdqueue, &cycles, 0);
//}

void lcd_thread_setup()
{
	lcdqueue = xQueueCreate(143, sizeof(int));
	xTaskCreatePinnedToCore(&render_line, "renderScanline", 4096, NULL, 5, NULL, 0);
}
