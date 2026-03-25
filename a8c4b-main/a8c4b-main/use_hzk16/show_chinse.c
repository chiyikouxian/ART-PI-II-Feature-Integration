#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <string.h>
#include "font_8x16.h" 
#include <unistd.h>


static struct fb_var_screeninfo var;
int pixel_width;
int line_width;
int screen_size;
unsigned char* fb_mem;

struct stat statbuf;
unsigned char* hzk16_mem;

//描点函数
void lcd_put_pixel(int x,int y,int color);
//显示ASCII码字符
void lcd_put_ascii(int x,int y,char c);
//显示ASCII码字符串
void lcd_put_str(int x,int y, char *s);
//显示中文字符
void lcd_put_chinese(int x,int y,char * s);

int main(int argc,char *argv[])
{
	//1.打开framebuffer设备
	int fd_fb = open("/dev/fb0",O_RDWR);
	if(fd_fb == -1)
	{
		printf("can not open /dev/fb0 \n");
		return -1;
	}

	//2.获取framebuffer参数
	if(ioctl(fd_fb,FBIOGET_VSCREENINFO,&var))
	{
		printf("can not get fb var\n");
		return -1;
	}
	pixel_width = var.bits_per_pixel / 8;
	line_width = pixel_width * var.xres;
	screen_size = line_width * var.yres;

	//3.将framebuffer映射到内存
	fb_mem = (unsigned char*)mmap(NULL,screen_size,PROT_READ | PROT_WRITE,MAP_SHARED,fd_fb,0);
	if(fb_mem == (unsigned char*) -1)
	{
		printf("can not mmap\n");
		return -1;
	}

	//4.绘制
	/* 清屏 */
	memset(fb_mem,0xff,screen_size);
 	
	//描点函数
	int i;
 	for(i=0;i<100;++i)
	{
		lcd_put_pixel(var.xres/5 + i,var.yres/5,0xff0000);
	}
	
	//显示ASCII码字符
	lcd_put_ascii(var.xres/3,var.yres/3,'1');
	
	//显示ASCII码字符串
 	//lcd_put_str(var.xres/2,var.yres/2,"ASD123");
	
	//显示中文字符串
	//1.打开HZK16文件
	int fd_hzk16=open("HZK16",O_RDONLY);
	if(fd_hzk16 < 0)
	{
		printf("can not open HZK16\n");
		return -1;
	}

	//2.计算HZK16文件大小
	if(fstat(fd_hzk16,&statbuf))
	{
		printf("can not get stat\n");
		return -1;
	}
	

	//3.将HZK16文件映射到内存
	hzk16_mem =(unsigned char*) mmap(NULL,statbuf.st_size,PROT_READ,MAP_SHARED,fd_hzk16,0);
	if(hzk16_mem == (unsigned char*)-1)
	{
		printf("can not map hzk16\n");
		return -1;
	}
	
	//4.显示中文字符
	unsigned char str[]="中";
	lcd_put_chinese(var.xres*3/5,var.yres*2/5,str);
	
	//5.显示混合字符串
	lcd_put_str(var.xres/2,var.yres/2,"中国加油\nfighting");	
	return 0;
}

//描点函数
void lcd_put_pixel(int x,int y,int color)
{
	unsigned char *pen_8 = fb_mem + line_width * y + pixel_width * x;
	unsigned short *pen_16;
	unsigned int *pen_32;

	int red,green,blue;
	pen_16 = (unsigned short*)pen_8;
	pen_32 = (unsigned int*)pen_8;

	switch(var.bits_per_pixel)
	{
		case 8:
			*pen_8 = color;
			break;
		case 16:
			{
				/* 565 */
				red = (color>>16) & 0xff;
				green = (color>>8) & 0xff;
				blue = (color>>0) & 0xff;

				*pen_16 = ((red>>3)<<11) | ((green>>2)<<5) | (blue>>3);
			}
			break;
		case 32:
			*pen_32 = color;
			break;
		default:
			printf("can not support %d bpp\n",var.bits_per_pixel);
			break;
	}
}

//显示ASCII码字符
void lcd_put_ascii(int x,int y,char c)
{
	unsigned char* fontdata = (unsigned char*)&fontdata_8x16[c*16];
	unsigned char byte;

	int i,j;
	for(i=0;i<16;++i)
	{
		byte = fontdata[i];

		for(j=7;j>=0;--j)
		{
			if(byte & (1<<j))
				lcd_put_pixel(x+7-j,y+i,0xffffff);  //白
			else
				lcd_put_pixel(x+7-j,y+i,0);  //黑
		}
	}
}

//显示ASCII码字符串
void lcd_put_str(int x,int y, char *s)
{
	int len = strlen(s);
	int i;
	int tempx = x;

	for(i=0;i<len;)
	{
		if(s[i] == '\n')
		{
			x = tempx;
			y += 16;
			printf("huan hang\n");
			i += 1;
		}
		else
		{
			if(((unsigned char)s[i]) < 0x80)
			{
				lcd_put_ascii(x,y,s[i]);
				x += 8;
				i += 1;
			}
			else
			{
				lcd_put_chinese(x,y,&s[i]);
				x += 16;
				i += 2;
			}
		}
	}
}


//显示中文字符
void lcd_put_chinese(int x,int y,char * s)
{
	unsigned int area = s[0] - 0xA1;
	unsigned int where = s[1] - 0xA1;
	unsigned char * dots = hzk16_mem +(area*94+where)*32;

	int i,j,k;
	unsigned char byte;
	for(i=0;i<16;++i)
	{
		for(j=0;j<2;++j)
		{
			byte = dots[i*2+j];
			for(k=7;k>=0;--k)
			{
				if(byte & (1<<k))
					lcd_put_pixel(x+8*j+7-k,y+i,0xffffff);
				else
					lcd_put_pixel(x+8*j+7-k,y+i,0);
			}
		}
	}
}




