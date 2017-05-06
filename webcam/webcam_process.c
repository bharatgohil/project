#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

/*TODO:Remove this of friendlyarm*/
/*#define ORANGE_PI*/

#define DATA_LOC	"/media/storage"
#define MEDIA_DEVICE	"/dev/sda1"
#define VIDEO_DEVICE	"/dev/video1"
#define FFMPEG_STR "ffmpeg -loglevel quiet -f v4l2 -framerate %d -video_size %s -input_format h264 -i "VIDEO_DEVICE" -t %d -vcodec copy %s/%s.mp4"
#define FPS 		16
#define RESOLUTION 	"1280x720"
#define FILE_DURATION	60
#define MAX_FFMPEG_STR 	1024
#define MIN_STORE_SPACE 256*1024*1024
#define MAX_SYS_PATH	1024
#define MOTION_REC_TIMEOUT 35
#define NORMAL_RECORD_ONTIME 1
#define NORMAL_RECORD_OFFTIME 3
#define PREVIEW_OFFTIME 1
#define PREVIEW_ONTIME 3
#define BOOT_COMPLATE_ONTIME 1
#define WAIT_TIME_PHONE_TO_BOARD 5
#define START_RECORD_HOLD_TIME 60

#ifdef ORANGE_PI
#define EXPORT_PATH "/sys/class/gpio_sw/export"
#define DIRECTION_PATH "/sys/class/gpio_sw/PA%d/cfg"
#define INOUT_PATH "/sys/class/gpio_sw/PA%d/data"
#else
#define EXPORT_PATH "/sys/class/gpio/export"
#define DIRECTION_PATH "/sys/class/gpio/gpio%d/direction"
#define INOUT_PATH "/sys/class/gpio/gpio%d/value"
#endif

enum input_no{
	MSD_BOARD_IN = 7,
	MSD_PHONE_IN,
	START_RECORD_MOTION_IN,
};

enum output_no{
	STATUS_RECORD_OUT_FA = 10,
	STATUS_RECORD_OUT = 20,
	PHONE_BOARD_OUT	= 21,
};

enum input_output{
	INPUT,
	OUTPUT,
};

static volatile uint32_t run_ffmpeg;

enum on_off{
	OFF,
	ON,
};

enum board_phone{
	BOARD,
	PHONE,
};

static volatile uint32_t record_timeout,msd_board_or_phone;
static volatile uint8_t on_time, off_time;

static void sysfs_gpio_export(uint8_t gpio_pin)
{
	int32_t gpio_fd;
	char gpio_path[MAX_SYS_PATH];
	char gpio_str_val[2];

	snprintf(gpio_path, MAX_SYS_PATH, EXPORT_PATH);
	if((gpio_fd = open(gpio_path, O_RDWR)) < 0)
	{
		perror("open:gpio export failed");
		return;
	}
	sprintf(gpio_str_val, "%d", gpio_pin);
	if(write(gpio_fd, gpio_str_val, strlen(gpio_str_val)) < 0)
	{
		perror("write:gpio export failed");
		return;
	}

	close(gpio_fd);
}

static void sysfs_gpio_set_direction(uint8_t gpio_pin, uint8_t value)
{
	int32_t gpio_fd;
	char gpio_path[MAX_SYS_PATH];
	char gpio_str_val[2];
	snprintf(gpio_path, MAX_SYS_PATH, DIRECTION_PATH, gpio_pin);
	if((gpio_fd = open(gpio_path, O_RDWR)) < 0)
	{
		fprintf(stderr, "open:gpio direction failed for %s\n", gpio_path);
		fprintf(stderr, "error=%s\n", strerror(errno));
		return;
	}
	#ifdef ORANGE_PI
		sprintf(gpio_str_val, "%d", value);
	#else
		if(value == INPUT)
			strncpy(gpio_str_val, "in", 3);
		else
			strncpy(gpio_str_val, "out", 4);
	#endif

	if(write(gpio_fd, gpio_str_val, strlen(gpio_str_val)) < 0)
	{
		fprintf(stderr, "write:gpio direction failed for %d\n", gpio_pin);
		return;
	}

	close(gpio_fd);
}

static uint8_t sysfs_gpio_get_io(uint8_t gpio_pin)
{
	int32_t gpio_fd;
	uint8_t value;
	char gpio_path[MAX_SYS_PATH];
	char gpio_str_val[2];
	snprintf(gpio_path, MAX_SYS_PATH, INOUT_PATH, gpio_pin);
	if((gpio_fd = open(gpio_path, O_RDWR)) < 0)
	{
		fprintf(stderr, "open:gpio get value failed for %d\n", gpio_pin);
		return -1;
	}

	if(read(gpio_fd, gpio_str_val, 1) < 0)
	{
		fprintf(stderr, "read:gpio get vlaue failed for %d\n", gpio_pin);
		return -1;
	}
	gpio_str_val[1]= '\0';
	close(gpio_fd);

/***********************/
	if(gpio_str_val[0] == '0')
		value = OFF;
	else
		value = ON;
/***********************/
	return value;
}

static void sysfs_gpio_set_io(uint8_t gpio_pin, uint8_t value)
{
	int32_t gpio_fd;
	char gpio_path[MAX_SYS_PATH];
	char gpio_str_val[2];

	snprintf(gpio_path, MAX_SYS_PATH, INOUT_PATH, gpio_pin);
	if((gpio_fd = open(gpio_path, O_RDWR)) < 0)
	{
		fprintf(stderr, "open:gpio set value failed for %d\n", gpio_pin);
		return;
	}

	sprintf(gpio_str_val, "%d", value);
	if(write(gpio_fd, gpio_str_val, strlen(gpio_str_val)) < 0)
	{
		fprintf(stderr, "write:gpio set vlaue failed for %d\n", gpio_pin);
		return;
	}

	close(gpio_fd);

}

static void sysfs_gpio_unexport(uint8_t gpio_pin)
{
	int32_t gpio_fd;
	char gpio_path[MAX_SYS_PATH];
	snprintf(gpio_path, MAX_SYS_PATH, "/sys/class/gpio_sw/unexport");
	if((gpio_fd = open(gpio_path, O_RDWR)) < 0)
	{
		fprintf(stderr, "open:gpio unexport failed for %d\n", gpio_pin);
		return;
	}

	if(write(gpio_fd, &gpio_pin, sizeof(gpio_pin)) < 0)
	{
		fprintf(stderr, "write:gpio unexport failed for %d\n", gpio_pin);
		return;
	}

	close(gpio_fd);

}

static void output_data(uint32_t io_num, uint32_t out)
{
	char *phone_board_str[2] = {"board", "phone"};

	fprintf (stderr, "switching MSD to %s!\n", phone_board_str[out]);
	sysfs_gpio_set_io(PHONE_BOARD_OUT, out);

}

static int32_t check_input()
{
	struct stat stat_fl;

	return stat(VIDEO_DEVICE, &stat_fl);
}

static int32_t check_media()
{
	struct stat stat_fl;

	return stat(MEDIA_DEVICE, &stat_fl);
}

static void* motion_timeout_thread(void* th_data)
{
	do{

		if(record_timeout > 0)
			record_timeout--;
		sleep(1);
	}while(1);
}

static void* record_thread(void* th_data)
{
	char str_buf[MAX_FFMPEG_STR];
	struct statvfs statvfs_buf;
	unsigned long long size_var;
	char filename[40];
	struct tm *time_var;
	time_t now;

	do{
			if( (check_input() != 0) || (check_media() != 0) )
			{
				fprintf(stderr, "Mass storage or video camera missing\n");
				continue;
			}

			if(mount(MEDIA_DEVICE, DATA_LOC, "vfat", 0, NULL) != 0)
			{
				fprintf(stderr, "Mount failed.Please insert fat formatted usb drive\n");
				sleep(1);
				continue;
			}
	}while(0);

	do
	{
		if(statvfs(DATA_LOC, &statvfs_buf) != 0)
		{
			sleep(1);
			if(umount(DATA_LOC) != 0)
				fprintf(stderr, "umount failed\n");
			continue;
		}

		size_var = statvfs_buf.f_bfree*statvfs_buf.f_bsize;
		if (size_var < MIN_STORE_SPACE)
		{
			fprintf (stderr, "Only %llu free size on device. Please make more space and start system\n", size_var);
			if(umount(DATA_LOC) != 0)
				fprintf(stderr, "umount failed\n");
			sleep(1);
			continue;
		}

		if(record_timeout)
		{
			now = time(NULL);
			time_var = localtime (&now);
			strftime(filename, 40, "%m-%d-%Y_%H-%M-%S", time_var);

			snprintf(str_buf, MAX_FFMPEG_STR, FFMPEG_STR, FPS, RESOLUTION, FILE_DURATION, DATA_LOC, filename);
			system(str_buf);
			/*off_time = NORMAL_RECORD_OFFTIME;*/
			system("echo 3 > /proc/sys/vm/drop_caches");
			fprintf(stderr, "Transfering data RAM to MSD...\n");
			sync();
			fprintf(stderr, "Transfering data RAM to MSD complate\n");
		}
		else
			sleep(1);
	}while(run_ffmpeg);

	if(umount(DATA_LOC) != 0)
		fprintf(stderr, "umount failed\n");
 fprintf(stderr, "Recording stopped\n");
	return NULL;
}

static int32_t record_cam(uint32_t start_stop)
{
	static pthread_t thread_id;

	switch(start_stop)
	{
	case 0:
		/*Send shutdown signal to ffmpeg process*/

		if (run_ffmpeg == 1)
		{
			run_ffmpeg = 0;
			system("killall -INT ffmpeg");

			/*wait till thread exits*/
			while(pthread_kill(thread_id, 0) == 0)
			{
				sleep(1);
			}
		}
		break;

	case 1:
		/*Start ffmpeg process*/
		if( run_ffmpeg == 0 )
		{
			run_ffmpeg = 1;
			if(pthread_create(&thread_id, NULL, (void*)&record_thread, NULL) < 0)
			{
				fprintf (stderr, "Thread creation failed!\n");
				return 1;
			}
		}
		break;
	default:
		break;
	}
}

void * set_record_status(void * data)
{
	while(1)
	{
			sysfs_gpio_set_io(STATUS_RECORD_OUT, ON);
			sysfs_gpio_set_io(STATUS_RECORD_OUT_FA, ON);
			sleep(on_time);
			sysfs_gpio_set_io(STATUS_RECORD_OUT, OFF);
			sysfs_gpio_set_io(STATUS_RECORD_OUT_FA, OFF);
			sleep(off_time);
	}
}

int32_t cleanup_msd(void)
{
	char system_cmd[MAX_SYS_PATH];
	mkdir(DATA_LOC, 0777);
	if(mount(MEDIA_DEVICE, DATA_LOC, "vfat", 0, NULL) != 0)
	{
		fprintf(stderr, "Mount failed.Please insert fat formatted usb drive\n");
		sleep(1);
		return -1;
	}

	fprintf(stderr, "Successfully Mounted disk\n");
	snprintf(system_cmd, MAX_SYS_PATH, "rm -rf %s/*", DATA_LOC);
	system(system_cmd);
	sync();
	if(umount(DATA_LOC) != 0)
		fprintf(stderr, "umount failed\n");
	fprintf(stderr, "Remove all content from disk\n");
	return 0;
}

static int32_t read_live_record_button()
{
	uint32_t status = 0, start_record_hold_time;
	uint8_t pressed_time = 0;
	if(sysfs_gpio_get_io(MSD_BOARD_IN) == OFF)
	{
		on_time = NORMAL_RECORD_ONTIME;
		off_time = NORMAL_RECORD_OFFTIME;
		fprintf(stderr, "Valid Start record pressed\n");

		do{
				;
		}while (sysfs_gpio_get_io(MSD_BOARD_IN) == OFF);
		fprintf(stderr, "Valid Start record released\n");
		if(0x1 == (msd_board_or_phone & 0x1))
		{
			fprintf(stderr, "Already in recording state!Ingoring input\n");
			return status;
		}

		start_record_hold_time = START_RECORD_HOLD_TIME;
		fprintf(stderr, "Lets pause for %d seconds\n",START_RECORD_HOLD_TIME);
		do{
			if(sysfs_gpio_get_io(MSD_PHONE_IN) == OFF)
			{
				fprintf(stderr, "start recording interrupted\n");
				msd_board_or_phone = 0;
				return status;
			}
			sleep(1);
		}while(--start_record_hold_time);
		if(cleanup_msd() != 0)
			return status;
		msd_board_or_phone = 1;
		status = 0x1;
		fprintf(stderr, "Entering into recording state!wating for motion to detect\n");
	}
	else
	{
		if(sysfs_gpio_get_io(MSD_PHONE_IN) == OFF)
		{
			fprintf(stderr, "Request for MSD->phone\n");
			msd_board_or_phone = 0;
			sleep(1);
			do{
					;
			}while (sysfs_gpio_get_io(MSD_PHONE_IN) == OFF);
			status = 0x1;
		}
 	}

	if(sysfs_gpio_get_io(START_RECORD_MOTION_IN) == OFF)
	{
		do{
				;
		}while (sysfs_gpio_get_io(START_RECORD_MOTION_IN) == OFF);

		if(0x1 == (msd_board_or_phone & 0x1))
		{
			msd_board_or_phone = msd_board_or_phone | 0x2;
			status = 0x1;
		}
	}

	return status;
}

void switch_msd_to_phone()
{
	on_time = PREVIEW_ONTIME;
	off_time = PREVIEW_OFFTIME;
	record_cam(OFF);
	/*Connect MSD to phone*/
	output_data(PHONE_BOARD_OUT, PHONE);
}

static uint8_t switch_msd_to_board(void)
{
	on_time = NORMAL_RECORD_ONTIME;
	off_time = NORMAL_RECORD_OFFTIME;
	/*Output to switch from board to phone*/
	output_data(PHONE_BOARD_OUT, BOARD);

	/*wait till mass storage device gets stabilize*/
	sleep(2);

	record_cam(ON);
}

int32_t main()
{
	uint32_t tmp_push_button;
	pthread_t get_in_th_id, set_status_th_id,motion_th_id;

	#ifdef ORANGE_PI
	;
	#else
	sysfs_gpio_export(MSD_BOARD_IN);
	usleep(10000);
	sysfs_gpio_export(MSD_PHONE_IN);
	usleep(10000);
	sysfs_gpio_export(START_RECORD_MOTION_IN);
	usleep(10000);
	sysfs_gpio_export(STATUS_RECORD_OUT);
	usleep(10000);
	sysfs_gpio_export(PHONE_BOARD_OUT);
	usleep(10000);
	sysfs_gpio_export(STATUS_RECORD_OUT_FA);
	usleep(10000);
	#endif
	sysfs_gpio_set_direction(MSD_BOARD_IN, INPUT);
	sysfs_gpio_set_direction(MSD_PHONE_IN, INPUT);
	sysfs_gpio_set_direction(START_RECORD_MOTION_IN, INPUT);
	sysfs_gpio_set_direction(STATUS_RECORD_OUT, OUTPUT);
	sysfs_gpio_set_direction(PHONE_BOARD_OUT, OUTPUT);
	sysfs_gpio_set_direction(STATUS_RECORD_OUT_FA, OUTPUT);

	sysfs_gpio_set_io(STATUS_RECORD_OUT, ON);
	sysfs_gpio_set_io(STATUS_RECORD_OUT_FA, ON);
	output_data(PHONE_BOARD_OUT, BOARD);
	sleep(1);

	do{

		if( (check_input() != 0) || (check_media() != 0) )
		{
			fprintf(stderr, "sutable input/output device not found.Please connect and reboot the board\n");
			sleep(1);
			continue;
		}
		fprintf(stderr, "Valid input devices found\n");


	on_time = NORMAL_RECORD_ONTIME;
	off_time = BOOT_COMPLATE_ONTIME;

	if(pthread_create(&set_status_th_id, NULL, set_record_status, NULL) != 0)
			fprintf(stderr, "Set record status thread failed\n");

	if(pthread_create(&motion_th_id, NULL, motion_timeout_thread, NULL) != 0)
			fprintf(stderr, "motion timeout thread failed\n");

	do{
			/*Check every 500msec*/
			usleep(500000);
			if(1 == read_live_record_button())
			{
				switch(msd_board_or_phone)
				{
					case 0:
						fprintf(stderr, "Requesting for preview\n");
						/*Connect MSD to phone*/
						switch_msd_to_phone ();
					break;

					case 3:
						fprintf(stderr, "Motion Detected!Requesting for recording\n");
						record_timeout = MOTION_REC_TIMEOUT;
						switch_msd_to_board ();
						/*Connect MSD to board*/
						break;
					default:
						fprintf(stderr, "msd_board_or_phone val = %d\n", msd_board_or_phone);
						break;
				}
			}
		}while(1);
	}while(0);
	return 0;
}
