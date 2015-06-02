/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: user_main.c
 *
 * Description: Main routines for MP3 decoder.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
*******************************************************************************/
#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "../mad/mad.h"
#include "../mad/stream.h"
#include "../mad/frame.h"
#include "../mad/synth.h"
#include "i2s_freertos.h"
#include "spiram_fifo.h"

//The server to connect to
#define server_ip "192.168.12.1"
//#define server_ip "192.168.40.117"
//#define server_ip "192.168.1.4"
//#define server_ip "192.168.4.100"
#define server_port 1234



#define AP_NAME "testjmd"
#define AP_PASS "pannenkoek"
/*
#define AP_NAME "wifi-2"
#define AP_PASS "thesumof6+6=12"
*/


//Priorities of the reader and the decoder thread. Higher = higher prio.
#define PRIO_READER 2
#define PRIO_MAD 3


#define PWM_HACK
#ifdef PWM_HACK
const unsigned int ICACHE_RODATA_ATTR fakePwm[]={ 0x00000010, 0x00000410, 0x00400410, 0x00400C10, 0x00500C10, 0x00D00C10, 0x20D00C10, 0x21D00C10, 0x21D80C10, 
	0xA1D80C10, 0xA1D80D10, 0xA1D80D30, 0xA1DC0D30, 0xA1DC8D30, 0xB1DC8D30, 0xB9DC8D30, 0xB9FC8D30, 0xBDFC8D30, 0xBDFE8D30, 
	0xBDFE8D32, 0xBDFE8D33, 0xBDFECD33, 0xFDFECD33, 0xFDFECD73, 0xFDFEDD73, 0xFFFEDD73, 0xFFFEDD7B, 0xFFFEFD7B, 0xFFFFFD7B, 
	0xFFFFFDFB, 0xFFFFFFFB, 0xFFFFFFFF};
#endif


//The mp3 read buffer. 2106 bytes should be enough for up to 48KHz mp3s according to the sox sources. Used by libmad.
#define READBUFSZ (2106+64)
static char readBuf[READBUFSZ]; 

//This routine is called by the NXP modifications of libmad. It passes us (for the mono synth)
//32 16-bit samples.
void render_sample_block(short *short_sample_buff, int no_samples) {
	int i;
	int samp;

	for (i=0; i<no_samples; i++) {
#ifndef PWM_HACK
		//We can send a 32-bit sample to the I2S subsystem and the DAC will neatly split it up in 2
		//16-bit analog values, one for left and one for right.

		//Duplicate 16-bit sample to both the L and R channel
		samp=(short_sample_buff[i]);
		samp=(samp)&0xffff;
		samp=(samp<<16)|samp;
#else
		//Okay, when this is enabled it means a speaker is connected *directly* to the data output. Instead of
		//having a nice PCM signal, it's best to fake a PWM signal here.
		static int err=0;
		samp=short_sample_buff[i];
		samp=(samp+32768);	//to unsigned
		samp-=err;			//Add the error we made when rounding the previous sample (error diffusion)
		//clip value
		if (samp>65535) samp=65535;
		if (samp<0) samp=0;
		//send pwm value for sample value
		samp=fakePwm[samp>>11];
		err=(samp&0x7ff);	//Save rounding error.
#endif
		//Send the sample.
		i2sPushSample(samp);
	}
}

//Called by the NXP modificationss of libmad. Sets the needed output sample rate.
void set_dac_sample_rate(int rate) {
//	printf("sr %d\n", rate);
}

static enum  mad_flow ICACHE_FLASH_ATTR input(struct mad_stream *stream) {
	int n, i;
	int rem, fifoLen;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

	while (rem<sizeof(readBuf)) {
		n=(sizeof(readBuf)-rem); 	//Calculate amount of bytes we need to fill buffer.
		i=spiRamFifoFill();
		if (i<n) n=i; 				//If the fifo can give us less, only take that amount
		if (n==0) {					//Can't take anything?
			//Wait until there is enough data in the buffer. This only happens when the data feed 
			//rate is too low, and shouldn't normally be needed!
			printf("Buf uflow, need %d bytes\n", sizeof(readBuf)-rem);
			//We both silence the output as well as wait a while by pushing silent samples into the i2s system.
			//This waits for about 200mS
			for (n=0; n<441*2; n++) i2sPushSample(0);
		} else {
			//Read some bytes from the FIFO to re-fill the buffer.
			spiRamFifoRead(&readBuf[rem], n);
			rem+=n;
		}
	}

	//Okay, let MAD decode the buffer.
	mad_stream_buffer(stream, readBuf, sizeof(readBuf));
	return MAD_FLOW_CONTINUE;
}

//Routine to print out an error
static enum mad_flow ICACHE_FLASH_ATTR error(void *data, struct mad_stream *stream, struct mad_frame *frame) {
	printf("dec err 0x%04x (%s)\n", stream->error, mad_stream_errorstr(stream));
	return MAD_FLOW_CONTINUE;
}


//This is the main mp3 decoding task. It will grab data from the input buffer FIFO in the SPI ram and
//output it to the I2S port.
void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	int r;
	struct mad_stream *stream;
	struct mad_frame *frame;
	struct mad_synth *synth;

	//Allocate structs needed for mp3 decoding
	stream=malloc(sizeof(struct mad_stream));
	frame=malloc(sizeof(struct mad_frame));
	synth=malloc(sizeof(struct mad_synth));

	printf("MAD: Decoder start.\n");
	//Initialize mp3 parts
	mad_stream_init(stream);
	mad_frame_init(frame);
	mad_synth_init(synth);
	while(1) {
		input(stream); //calls mad_stream_buffer internally
		while(1) {
			r=mad_frame_decode(frame, stream);
			if (r==-1) {
	 			if (!MAD_RECOVERABLE(stream->error)) {
					//We're most likely out of buffer and need to call input() again
					break;
				}
				error(NULL, stream, frame);
				continue;
			}
			mad_synth_frame(synth, frame);
		}
	}
}



//Open a connection to a TCP port
int ICACHE_FLASH_ATTR openConn() {
	while(1) {
		int n, i;
		struct sockaddr_in remote_ip;
		int sock=socket(PF_INET, SOCK_STREAM, 0);
		if (sock==-1) {
//			printf("Client socket create error\n");
			continue;
		}
		bzero(&remote_ip, sizeof(struct sockaddr_in));
		remote_ip.sin_family = AF_INET;
		remote_ip.sin_addr.s_addr = inet_addr(server_ip);
		remote_ip.sin_port = htons(server_port);
//		printf("Connecting to client...\n");
		if (connect(sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))!=00) {
			close(sock);
			printf("Conn err.\n");
			vTaskDelay(1000/portTICK_RATE_MS);
			continue;
		}
		return sock;
	}
}


//Reader task. This will try to read data from a TCP socket into the SPI fifo buffer.
void ICACHE_FLASH_ATTR tskreader(void *pvParameters) {
	int madRunning=0;
	char wbuf[64];
	int n, l, inBuf;
	int fd;
	while(1) {
		fd=openConn();
		printf("Reading into SPI RAM FIFO...\n");
		do {
			n=read(fd, wbuf, sizeof(wbuf));
			if (n>0) spiRamFifoWrite(wbuf, n);
			
			if ((!madRunning) && (spiRamFifoFree()<=sizeof(wbuf))) {
				//Buffer is filled. Start up the MAD task. Yes, the 2100 bytes of stack is a fairly large amount but MAD seems to need it.
				if (xTaskCreate(tskmad, "tskmad", 2100, NULL, PRIO_MAD, NULL)!=pdPASS) printf("ERROR! Couldn't create MAD task! Out of memory?\n");
				madRunning=1;
			}
		} while (n>0);
		close(fd);
		printf("Read done, connection closed.\n");
	}
}

//Simple task to connect to an access point, initialize i2s and fire up the reader task.
void ICACHE_FLASH_ATTR tskconnect(void *pvParameters) {
	//Wait a few secs for the stack to settle down
	vTaskDelay(3000/portTICK_RATE_MS);
	
	//Go to station mode
	wifi_station_disconnect();
	if (wifi_get_opmode() != STATION_MODE) { 
		wifi_set_opmode(STATION_MODE);
	}

	//Connect to the defined access point.
	struct station_config *config=malloc(sizeof(struct station_config));
	memset(config, 0x00, sizeof(struct station_config));
	sprintf(config->ssid, AP_NAME);
	sprintf(config->password, AP_PASS);
	wifi_station_set_config(config);
	wifi_station_connect();
	free(config);

	//Initialize I2S and fire up the reader task. The reader task will fire up the MP3 decoder as soon
	//as it has read enough MP3 data.
	i2sInit();
	if (xTaskCreate(tskreader, "tskreader", 230, NULL, PRIO_READER, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");
	//We're done. Delete this task.
	vTaskDelete(NULL);
}

//We need this to tell the OS we're running at a higher clock frequency.
extern void os_update_cpu_frequency(int mhz);

void ICACHE_FLASH_ATTR
user_init(void)
{
	//Tell hardware to run at 160MHz instead of 80MHz
	//Disabled because we don't need 160MHz to do something puny like decoding an MP3 file.
#if 0
	SET_PERI_REG_MASK(0x3ff00014, BIT(0));
	os_update_cpu_frequency(160);
#endif
	
	//Set the UART to 115200 baud
	UART_SetBaudrate(0, 115200);

	//Initialize the SPI RAM chip communications and see if it actually retains some bytes. If it
	//doesn't, warn user.
	if (!spiRamFifoInit()) {
		printf("SPI RAM chip does not seem to work. Is it connected correctly?\n");
		while(1);
	}
	xTaskCreate(tskconnect, "tskconnect", 200, NULL, 3, NULL);
}

