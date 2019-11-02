#include "libretro.h"
#include "libretro-hatari.h"
#include "graph.h"
#include "vkbd.h"
#include "joy.h"
#include "screen.h"
#include "video.h"	/* FIXME: video.h is dependent on HBL_PALETTE_LINES from screen.h */

//CORE VAR
extern const char *retro_save_directory;
extern const char *retro_system_directory;
extern const char *retro_content_directory;
char RETRO_DIR[512];
char RETRO_TOS[512];

//HATARI PROTOTYPES
#include "configuration.h"
#include "file.h"
extern bool Dialog_DoProperty(void);
extern void Screen_SetFullUpdate(void);
extern void Main_HandleMouseMotion(void);
extern void Main_UnInit(void);
extern int  hmain(int argc, char *argv[]);
extern int Reset_Cold(void);

//TIME
#ifdef __CELLOS_LV2__
#include "sys/sys_time.h"
#include "sys/timer.h"
#define usleep  sys_timer_usleep
#else
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#endif

long frame=0;
static unsigned long Ktime=0, LastFPSTime=0;

//VIDEO
extern SDL_Surface *sdlscrn;
unsigned short int bmp[1024*1024];
unsigned char savbkg[1024*1024* 2];

//SOUND
short signed int SNDBUF[1024*2];
int snd_sampler = 44100 / 50;

//PATH
char RPATH[512];

//EMU FLAGS
int NPAGE=-1, KCOL=1, BKGCOLOR=0, MAXPAS=6;
int SHIFTON=-1,MOUSEMODE=-1,NUMJOY=0,SHOWKEY=-1,PAS=4,STATUTON=-1;
int SND; //SOUND ON/OFF
static int firstps=0;
int pauseg=0; //enter_gui

//JOY
int al[2];//left analog1
int ar[2];//right analog1
unsigned char MXjoy0; // joy
int NUMjoy=1;

//MOUSE
int touch=-1; // gui mouse btn
int fmousex,fmousey; // emu mouse
extern int gmx,gmy; //gui mouse

//KEYBOARD
char Key_Sate[512];
char Key_Sate2[512];

static int mbt[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

//STATS GUI
extern int LEDA,LEDB,LEDC;
int BOXDEC= 32+2;
int STAT_BASEY;

static retro_input_state_t input_state_cb;
static retro_input_poll_t input_poll_cb;

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

long GetTicks(void)
{ // in MSec
#ifndef _ANDROID_

#ifdef __CELLOS_LV2__

   //#warning "GetTick PS3\n"

   unsigned long	ticks_micro;
   uint64_t secs;
   uint64_t nsecs;

   sys_time_get_current_time(&secs, &nsecs);
   ticks_micro =  secs * 1000000UL + (nsecs / 1000);

   return ticks_micro/1000;
#else
   struct timeval tv;
   gettimeofday (&tv, NULL);
   return (tv.tv_sec*1000000 + tv.tv_usec)/1000;
#endif

#else

   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (now.tv_sec*1000000 + now.tv_nsec/1000)/1000;
#endif

}

#ifdef WIIU
#include <features_cpu.h>
retro_time_t current_tus=0,last_tus=0;
#endif
int slowdown=0;

//NO SURE FIND BETTER WAY TO COME BACK IN MAIN THREAD IN HATARI GUI
void gui_poll_events(void)
{
#ifdef WIIU
  current_tus=cpu_features_get_time_usec();
  current_tus/=1000;

   if(current_tus - last_tus >= 1000/50)
   {
      slowdown=0;
      frame++;
      last_tus = current_tus;
      co_switch(mainThread);
   }
#else
   Ktime = GetTicks();

   if(Ktime - LastFPSTime >= 1000/50)
   {
	  slowdown=0;
      frame++;
      LastFPSTime = Ktime;
      co_switch(mainThread);
   }
#endif
}

//save bkg for screenshot
void save_bkg(void)
{
   int i, j, k;
   unsigned char *ptr;

   k = 0;
   ptr = (unsigned char*)sdlscrn->pixels;

   for(j=0;j<retroh;j++)
   {
      for(i=0;i<retrow*2;i++)
      {
         savbkg[k]=*ptr;
         ptr++;
         k++;
      }
   }
}

void retro_fillrect(SDL_Surface * surf,SDL_Rect *rect,unsigned int col)
{
   DrawFBoxBmp(bmp,rect->x,rect->y,rect->w ,rect->h,col);
}

int  GuiGetMouseState( int * x,int * y)
{
   *x=gmx;
   *y=gmy;
   return 0;
}

void texture_uninit(void)
{
   if(sdlscrn)
   {
      if(sdlscrn->format)
         free(sdlscrn->format);

      free(sdlscrn);
   }
}

SDL_Surface *prepare_texture(int w,int h,int b)
{
   SDL_Surface *bitmp;

   if(sdlscrn)
      texture_uninit();

   bitmp = (SDL_Surface *) calloc(1, sizeof(*bitmp));
   if (bitmp == NULL)
   {
      printf("tex surface failed");
      return NULL;
   }

   bitmp->format = calloc(1,sizeof(*bitmp->format));
   if (bitmp->format == NULL)
   {
      printf("tex format failed");
      return NULL;
   }

   bitmp->format->BitsPerPixel = 16;
   bitmp->format->BytesPerPixel = 2;
   bitmp->format->Rloss=3;
   bitmp->format->Gloss=3;
   bitmp->format->Bloss=3;
   bitmp->format->Aloss=0;
   bitmp->format->Rshift=11;
   bitmp->format->Gshift=6;
   bitmp->format->Bshift=0;
   bitmp->format->Ashift=0;
   bitmp->format->Rmask=0x0000F800;
   bitmp->format->Gmask=0x000007E0;
   bitmp->format->Bmask=0x0000001F;
   bitmp->format->Amask=0x00000000;
   bitmp->format->colorkey=0;
   bitmp->format->alpha=0;
   bitmp->format->palette = NULL;

   bitmp->flags=0;
   bitmp->w=w;
   bitmp->h=h;
   bitmp->pitch=retrow*2;
   bitmp->pixels=(unsigned char *)&bmp[0];
   bitmp->clip_rect.x=0;
   bitmp->clip_rect.y=0;
   bitmp->clip_rect.w=w;
   bitmp->clip_rect.h=h;

   //printf("fin prepare tex:%dx%dx%d\n",bitmp->w,bitmp->h,bitmp->format->BytesPerPixel);
   return bitmp;
}

void texture_init(void)
{
   memset(bmp, 0, sizeof(bmp));

   gmx=(retrow/2)-1;
   gmy=(retroh/2)-1;
}

void enter_gui(void)
{
   save_bkg();

   Dialog_DoProperty();
   pauseg=0;
}

void pause_select(void)
{
   if(pauseg==1 && firstps==0)
   {
      firstps=1;
      enter_gui();
      firstps=0;
   }
}

void Print_Statut(void)
{
   STAT_BASEY=CROP_HEIGHT;

   DrawFBoxBmp(bmp,0,CROP_HEIGHT,CROP_WIDTH,STAT_YSZ,RGB565(0,0,0));

   if(MOUSEMODE==-1)
      Draw_text(bmp,STAT_DECX,STAT_BASEY,0xffff,0x8080,1,2,40,"Joy  ");
   else
      Draw_text(bmp,STAT_DECX,STAT_BASEY,0xffff,0x8080,1,2,40,"Mouse");

   Draw_text(bmp,STAT_DECX+40 ,STAT_BASEY,0xffff,0x8080,1,2,40,(SHIFTON>0?"SHFT":""));
   Draw_text(bmp,STAT_DECX+80 ,STAT_BASEY,0xffff,0x8080,1,2,40,"MS:%d",PAS);
   Draw_text(bmp,STAT_DECX+120,STAT_BASEY,0xffff,0x8080,1,2,40,"Joy:%d",NUMjoy);

   if(LEDA)
   {
      DrawFBoxBmp(bmp,CROP_WIDTH-6*BOXDEC-6-16,CROP_HEIGHT-0,16,16,RGB565(0,7,0));//led A drive
      Draw_text(bmp,CROP_WIDTH-6*BOXDEC-6-16,CROP_HEIGHT-0,0xffff,0x0,1,2,40," A");
   }

   if(LEDB)
   {
      DrawFBoxBmp(bmp,CROP_WIDTH-7*BOXDEC-6-16,CROP_HEIGHT-0,16,16,RGB565(0,7,0));//led B drive
      Draw_text(bmp,CROP_WIDTH-7*BOXDEC-6-16,CROP_HEIGHT-0,0xffff,0x0,1,2,40," B");
   }

   if(LEDC)
   {
      DrawFBoxBmp(bmp,CROP_WIDTH-8*BOXDEC-6-16,CROP_HEIGHT-0,16,16,RGB565(0,7,0));//led C drive
      Draw_text(bmp,CROP_WIDTH-8*BOXDEC-6-16,CROP_HEIGHT-0,0xffff,0x0,1,2,40," C");
      LEDC=0;
   }

}

void retro_key_down(unsigned char retrok)
{
   IKBD_PressSTKey(retrok,1);
}

void retro_key_up(unsigned char retrok)
{
   IKBD_PressSTKey(retrok,0);
}

void Process_key(void)
{
   int i;
   for(i=0;i<320;i++)
   {
      Key_Sate[i]=input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0,i) ? 0x80: 0;

      if(SDLKeyToSTScanCode[i]==0x2a )
      {  //SHIFT CASE

         if( Key_Sate[i] && Key_Sate2[i]==0 )
         {
            if(SHIFTON == 1)
               retro_key_up(	SDLKeyToSTScanCode[i] );
            else if(SHIFTON == -1)
               retro_key_down(SDLKeyToSTScanCode[i] );

            SHIFTON=-SHIFTON;

            Key_Sate2[i]=1;

         }
         else if ( !Key_Sate[i] && Key_Sate2[i]==1 )Key_Sate2[i]=0;

      }
      else
      {
         if(Key_Sate[i] && SDLKeyToSTScanCode[i]!=-1  && Key_Sate2[i]==0)
         {
            retro_key_down(	SDLKeyToSTScanCode[i] );
            Key_Sate2[i]=1;
         }
         else if ( !Key_Sate[i] && SDLKeyToSTScanCode[i]!=-1 && Key_Sate2[i]==1 )
         {
            retro_key_up( SDLKeyToSTScanCode[i] );
            Key_Sate2[i]=0;

         }

      }
   }

}


static int mbL=0;
static int mbR=0;

void update_input(void)
{
	int i;

	input_poll_cb();

	//
	// -- Keyboard

	Process_key();

	//
	// -- Joystick

	MXjoy0=0;

	al[0] =(input_state_cb( 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X));///2;
	al[1] =(input_state_cb( 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y));///2;

	/* Directions */
	if (al[1] <= JOYRANGE_UP_VALUE)
		MXjoy0 |= ATARIJOY_BITMASK_UP;
	else if (al[1] >= JOYRANGE_DOWN_VALUE)
		MXjoy0 |= ATARIJOY_BITMASK_DOWN;

	if (al[0] <= JOYRANGE_LEFT_VALUE)
		MXjoy0 |= ATARIJOY_BITMASK_LEFT;
	else if (al[0] >= JOYRANGE_RIGHT_VALUE)
		MXjoy0 |= ATARIJOY_BITMASK_RIGHT;

	// D-pad
	if ( input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT ) )
		MXjoy0 |= ATARIJOY_BITMASK_LEFT;
	if ( input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT ) )
		MXjoy0 |= ATARIJOY_BITMASK_RIGHT;
	if ( input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP ) )
		MXjoy0 |= ATARIJOY_BITMASK_UP;
	if ( input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN ) )
		MXjoy0 |= ATARIJOY_BITMASK_DOWN;

	// Fire
	if( input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A ) ||
		input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B ) )
	{
		MXjoy0 |= ATARIJOY_BITMASK_FIRE;
	}

	//
	// -- Mouse

	fmousex = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	fmousey = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

	int mouse_l = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
	int mouse_r = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);

	if(mbL==0 && mouse_l)
	{
		mbL=1;
		Keyboard.bLButtonDown |= BUTTON_MOUSE;
	}
	else if(mbL==1 && !mouse_l)
	{
		Keyboard.bLButtonDown &= ~BUTTON_MOUSE;
		mbL=0;
	}

	if(mbR==0 && mouse_r)
	{
		mbR=1;
		Keyboard.bRButtonDown |= BUTTON_MOUSE;
	}
	else if(mbR==1 && !mouse_r)
	{
		Keyboard.bRButtonDown &= ~BUTTON_MOUSE;
		mbR=0;
	}

	Main_HandleMouseMotion();
}


void input_gui(void)
{
   int SAVPAS=PAS;

   input_poll_cb();

   int mouse_l;
   int mouse_r;
   int16_t mouse_x,mouse_y;
   mouse_x=mouse_y=0;

   //mouse/joy toggle
   if ( input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, 2) && mbt[2]==0 )
      mbt[2]=1;
   else if ( mbt[2]==1 && ! input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, 2) )
   {
      mbt[2]=0;
      MOUSEMODE=-MOUSEMODE;
   }

   if(slowdown>0)return;

   if(MOUSEMODE==1)
   {

      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
         mouse_x += PAS;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
         mouse_x -= PAS;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
         mouse_y += PAS;
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
         mouse_y -= PAS;
      mouse_l=input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
      mouse_r=input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);

      PAS=SAVPAS;

   }
   else
   {
      mouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
      mouse_y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
      mouse_l    = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
      mouse_r    = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
   }

   slowdown=1;

   static int mmbL = 0, mmbR = 0;

   if(mmbL==0 && mouse_l)
   {
      mmbL=1;
      touch=1;
   }
   else if(mmbL==1 && !mouse_l)
   {
      mmbL=0;
      touch=-1;
   }

   if(mmbR==0 && mouse_r)
      mmbR=1;
   else if(mmbR==1 && !mouse_r)
      mmbR=0;

   gmx+=mouse_x;
   gmy+=mouse_y;
   if (gmx<0)
      gmx=0;
   if (gmx>retrow-1)
      gmx=retrow-1;
   if (gmy<0)
      gmy=0;
   if (gmy>retroh-1)
      gmy=retroh-1;

}

