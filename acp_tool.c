#include<sys/socket.h>
#include<string.h>
#include<stdbool.h>
#include<netinet/in.h>
#include<stdio.h>
#include<stdlib.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<unistd.h>
#include<errno.h>
#include<sys/select.h>
#include<sys/time.h>
#include<unistd.h>
#include<sys/types.h>
#include <dirent.h>
#include<pthread.h>
#include <termios.h> /*PPSIX终端控制定义*/
#include "uni_databuf.h"

#define SERV_PORT 			5000
#define MAX_SIZE 			1024*40
#define CMD_MAX_LEN 		228
#define VCOM_MAX_LEN 		2048
#define READ_LEN 			9
#define WRITE_LEN 			9
#define READ_DIR_LEN 		13
#define SEND_DIR_LEN 		13
#define MAX_PATH_LENGTH		128

DataBufHandle handle = NULL;
char cmd_buf[CMD_MAX_LEN] = {0};
char file_buf[VCOM_MAX_LEN] = {0};
char buf[VCOM_MAX_LEN] = {0};
int total_len = 0;

int store_fd = -1;
bool got_store_len = false;
const char* store_name = NULL;
int store_len =0;

int send_fd = -1;
const char* send_name = NULL;
int send_len = 0;

const char* dir_name = NULL;
int file_list_count = 0;

struct sockaddr_in servaddr;
struct sockaddr_in clie_addr;
socklen_t clie_addr_len = sizeof(struct sockaddr_in);
int sockfd = -1;
int link_id = -1;
char files_path[100][MAX_PATH_LENGTH]={0};

static int  exit_fun = 1;
static int s_total_filecount = 0;
static int s_len_filename = 0;
static int total = 0;
static int s_total_len = 0;
static char *file = NULL;
static FILE *s_fp = NULL;
static int fd = -1;

static int ret=0;

pthread_t rndis_tid;
pthread_t vcom_tid;

/***@brief 设置串口通信速率
*@param fd 类型 int 打开串口的文件句柄
*@param speed 类型 int 串口速度
*@return void*/

int speed_arr[] = { B38400, B19200, B9600, B4800, B2400, B1200, B300,
     B38400, B19200, B9600, B4800, B2400, B1200, B300, };
int name_arr[] = {38400, 19200, 9600, 4800, 2400, 1200, 300,
     38400, 19200, 9600, 4800, 2400, 1200, 300, };
void set_speed(int fd, int speed)
{
    int i;
    int status;
    struct termios Opt;
    tcgetattr(fd, &Opt);
    for ( i= 0; i < (int)(sizeof(speed_arr) / sizeof(int)); i++)
	{
		if (speed == name_arr[i])
		{
			tcflush(fd, TCIOFLUSH);
			cfsetispeed(&Opt, speed_arr[i]);
			cfsetospeed(&Opt, speed_arr[i]);
			status = tcsetattr(fd, TCSANOW, &Opt);
			if (status != 0)
				perror("tcsetattr fd");
			return;
		}
		tcflush(fd,TCIOFLUSH);
	}
}
/**
*@brief 设置串口数据位，停止位和效验位
*@param fd 类型 int 打开的串口文件句柄*
*@param databits 类型 int 数据位 取值 为 7 或者8*
*@param stopbits 类型 int 停止位 取值为 1 或者2*
*@param parity 类型 int 效验类型 取值为N,E,O,,S
*/
int set_Parity(int fd,int databits,int stopbits,int parity)
{
    struct termios options;
    if ( tcgetattr( fd,&options) != 0)
    {
    perror("SetupSerial 1");
    return false;
    }

    options.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    options.c_cflag &= ~(CSIZE | PARENB);
    options.c_cflag |= CS8;

	/* Set input parity option */
	if (parity != 'n') {
		options.c_iflag |= INPCK;
	}
	
	options.c_cc[VTIME] = 15; // 15 seconds
	options.c_cc[VMIN] = 10;
	tcflush(fd,TCIFLUSH); /* Update the options and do it NOW */
	if (tcsetattr(fd,TCSANOW,&options) != 0)
	{
		perror("SetupSerial 3");
		return false;
	}
    return true;
}
/**
*@breif 打开串口
*/
int OpenDev(const char *Dev)
{
	int fd_d = open( Dev, O_RDWR | O_NOCTTY); //| O_NOCTTY | O_NDELAY

	if (-1 == fd_d)
	{
		perror("sdssgl Can't Open Serial Port.\n");
		return -1;
	}
	else{
		perror("sdssgl  Open Serial Port success.\n");
	}
	return fd_d;
}
int create_dir1(const char *sPathName)
{
	char DirName[256];
	strcpy(DirName, sPathName);
	int i, len = strlen(DirName);
	if(DirName[len-1] != '/')
	strcat(DirName, "/");
	len = strlen(DirName);
	for(i = 1; i < len; i++)
	{
		if (DirName[i]=='/')
		{
			DirName[i] = 0;
			if (access(DirName, F_OK) != 0)
			{
				if (mkdir(DirName, 0755) == -1)
				{
					printf("mkdir error");
					return -1;
				}
			}
			DirName[i] = '/';
		}
	}

	return   0;
}

void reset_all_flags()
{
    //fd = -1;
    ret = 0;
    send_fd = -1;
    send_name = NULL;
    send_len = 0;

    store_fd = -1;
    got_store_len = false;
    store_name = NULL;
    store_len =0;

    dir_name = NULL;
    file_list_count = 0;

    total_len = 0;

    memset(cmd_buf,0,sizeof(cmd_buf));
    memset(buf,0,sizeof(buf));
    lseek(fd,0,SEEK_SET);
}

static int get_file_count(const char* basePath)
{
	DIR *dir             = NULL;
	struct dirent *ptr   = NULL;
	char path[1024]      = {0};

	getcwd(path,sizeof(path));
	printf("current path: %s, basePath: %s,total:%d\n",path,basePath,total);
	dir = opendir(basePath);
	if(dir == NULL)
	{
		printf(">>>>dir not exist %s,errno is %d,pls input correct path<<<\n",basePath,errno);
		return -1;
	}

	errno = 0;
	while ((ptr = readdir(dir)) != NULL)
	{
		//顺序读取每一个目录项；
		//跳过“…”和“.”两个目录
		if (strcmp(ptr->d_name,".") == 0 || strcmp(ptr->d_name,"..") == 0)
		{
			continue;
		}

		//如果是目录，则递归调用 get_file_count函数

		if(ptr->d_type == 4)
		{
			memset(path,0,sizeof(path));
			sprintf(path,"%s/%s",basePath,ptr->d_name);

			printf("dir is %s\n",path);
			get_file_count(path);
		}

		if(ptr->d_type == 8)
		{
			memset(path,0,sizeof(path));
			sprintf(path,"%s/%s",basePath,ptr->d_name);
			strcpy(files_path[total++],path);
			printf("path %d is %s\n",total,path);
		}
	}

	if(errno != 0)
	{
		printf("fail to read dir:%d\n",errno); //脢搂掳脺脭貌脢盲鲁枚脤谩脢鸥脨脜脧垄
		return -1;
	}
	closedir(dir);
	return total;
}

void* send_vcom_file_loop(void *arg)
{
    while(1){
        reset_all_flags();
        fd_set read_fds;
        struct timeval time_out = {15,0};
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        int expected_len = 0;
        int real_len = 0;

        ret = select(fd+1, &read_fds, NULL, NULL, &time_out);
        if(ret < 0) {
            printf("select faild %d\n",ret);
            break;
        }
        else if(ret == 0)
        {
            printf("select send timeout\n");
            continue;
        }
        if(!FD_ISSET(fd, &read_fds))
        {
            printf("nothing can read  \n");
            //break;
        }
        while(1)
        {
			ret=read(fd,cmd_buf,sizeof(cmd_buf));
			//printf("read ret is %d\n",ret);
			expected_len += ret;
			if (expected_len >= 128) break;
        }

        real_len = strlen(cmd_buf);
        printf("real_len is %d,cmd is %s \n",real_len,cmd_buf);
        printf("%x %x %x %x %x %x %x %x %x %x\n",cmd_buf[0],cmd_buf[1],cmd_buf[2],cmd_buf[3],cmd_buf[4],cmd_buf[5],cmd_buf[6],cmd_buf[7],cmd_buf[8],cmd_buf[9]);

        if(strstr(cmd_buf,"read-req:")!=NULL)
        {
            send_name = cmd_buf+READ_LEN;
            printf("send name is %s\n",send_name);

            send_fd = open(send_name,O_RDONLY);
            if(send_fd == -1)
            {
                printf("open failed\n");
                break;
            }

            unsigned char buff4[4] ={0};
            send_len= lseek(send_fd,0L,SEEK_END);
            lseek(send_fd,0L,SEEK_SET);
            printf("send file size is %d\n",send_len);

            memcpy(buff4,&send_len,sizeof(buff4));
    		printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
            ret = write(fd,buff4,4);
            if(ret != 4)
            {
                printf("write length failed ret is %d\n",ret);
                break;
            }
			
            //printf("response success\n");
            sleep(1);
            total_len = send_len;
            //start_time = time(NULL);
            //printf("send_len is %d, start time is %d\n",send_len,start_time);
            while(1)
            {
                int len = DataBufferGetDataSize(handle);
                if( len > 0)
                {
                    DataBufferRead(buf,len,handle);
                    write(fd, buf, len);
                }

                ret= read(send_fd, buf, sizeof(buf));
                //printf("read length ret is %d\n",ret);
                if(ret <= -1) {
                    printf("read Error\n");
                    return;
                } else if(ret == 0) {
                    printf("read finished\n");
                    break;
                }

    			if (send_len <= VCOM_MAX_LEN) {
    				int i=0;
    				while (1) {
    					if (send_len >= 500) {
    						write(fd, buf+i, 500);
    						send_len -= 500;
    						i+=500;
    					} else {
    						write(fd, buf+i, send_len);
    						printf("write:%d\n", send_len);
    						break;
    					}
    				}
    			} else {
    				write(fd, buf, ret);
    				send_len-=ret;
    			}

                //printf("left length ret is %d\n",send_len);
                if (send_len <= 0) {
                    fsync(fd);
                    printf("sync!\n");
                    break;
    			}
                memset(buf,0,sizeof(buf));
            }
            if (send_fd)
            {
				close(send_fd);
				// printf("write2 success\n");
            }
        }
        else if (strstr(cmd_buf,"send-req:") != NULL)
        {
            store_name = cmd_buf+WRITE_LEN;
            printf("store name is %s\n",store_name);

            store_fd = open(store_name,O_WRONLY | O_CREAT |O_TRUNC);
            if (store_fd == -1)
            {
                printf("store file open failed\n");
                break;
            }

            while(1)
            {
                int len  = DataBufferGetDataSize(handle);
                if (len > 0)
                {
                    printf("recv status error \n");
                    break;
					// DataBufferRead(buf,len,handle);
                    //write(fd, buf, len);
                }
                if (!got_store_len)
                {
                    unsigned char buff[4];
    				memset(buff, 0, 4);
					
                    ret= read(fd, buff, sizeof(buff));
                    if(ret == 4)
                    {
						memcpy(&store_len,buff,sizeof(buff));
						total_len = store_len;
						//printf("store_len is %d, start time is %d\n",store_len,start_time);
						printf("store_len is %d\n",store_len);
						printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);
                    }
                    got_store_len = true;
                }
                // read file length
                ret= read(fd, buf, sizeof(buf));
                printf("read length ret is %d\n",ret);
                if (ret ==- 1)
                {
                    printf("read Error\n");
                    break;
                }
                else if(ret == 0)
                {
                    printf("read finished\n");
                    break;
                }

                write(store_fd, buf, ret);

                store_len-=ret;
                printf("store_len %d\n",store_len);
				// printf("left length ret is %d\n",store_len);
                if (store_len <= 0)
                {
                    break;
                }

                bzero(buf,sizeof(buf));
            }
            if(store_fd)
            {
                close(store_fd);
                printf("write success\n");
            }
        }
        else if (strstr(cmd_buf,"read-dir-req:")!=NULL)
        {
            char buff4[4] = {0};
            int rc = 0;
            int len = 0;
			
            dir_name = cmd_buf+READ_DIR_LEN;
            printf("dir_name is %s\n",dir_name);
            if ((file_list_count =  get_file_count(dir_name))==-1) return;

            memcpy(buff4,&file_list_count,sizeof(buff4));
            printf("file_list_count is %d\n",file_list_count);

            write(fd, buff4, sizeof(buff4));
            int i=0;
            for (i=0;i<file_list_count;++i)
            {
                //printf("file name is %s\n",(*iter).c_str());
                memset(cmd_buf,0,sizeof(cmd_buf));
                if (strlen(files_path[i]) >= sizeof(cmd_buf))
                {
                    printf("file name lenth too long %d\n", strlen(files_path[i]));
                    return;
                }
                strcpy(cmd_buf,files_path[i]);

                //send length of file name
#if 0
                len = strlen(cmd_buf);
                memcpy(buff4,&len,sizeof(buff4));
                printf("wk:%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
                write(fd, buff4, sizeof(buff4));
#endif
                printf("path %d to send :%s,%d\n",i,cmd_buf,strlen(cmd_buf));
                int err = write(fd, cmd_buf, sizeof(cmd_buf));
                if (err <= 0)
                {
                    printf("write Error\n");
                    return;
                }
                usleep(10000);
             }
            for (i=0;i<file_list_count;++i)
            {
                send_fd = open(files_path[i],O_RDONLY);
                if (send_fd == -1)
                {
                    printf("open failed\n");
                    return;
                }
                printf("open file %s\n",files_path[i]);

                send_len= lseek(send_fd,0L,SEEK_END);
                lseek(send_fd,0L,SEEK_SET);
                printf("send file size is %d\n",send_len);

                memcpy(buff4,&send_len,sizeof(buff4));
				printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
				sleep(1);
                //rc = send(link_id, buff4, 4, 0);
                rc = write(fd, buff4, sizeof(buff4));
                if (rc != 4)
                {
                    printf("write length failed ret is %d\n",rc);
                    return;
                }

                usleep(10000);

                total_len = send_len;
                while(1)
                {
					//usleep(100000);
					rc= read(send_fd, file_buf, sizeof(file_buf));
					//printf("read length ret is %d\n",ret);
                    if(rc <= -1) {
                        printf("read Error\n");
                        break;
                    } else if(rc == 0) {
                        printf("read finished\n");
                        break;
                    }

					//send(link_id, file_buf, rc, 0);
					rc = write(fd, file_buf, rc);
					send_len-=rc;

                    //printf("left length ret is %d\n",send_len);
                    if(send_len <= 0) {
                        printf("sync!\n");
                        break;
        	        }
                    memset(file_buf,0,sizeof(file_buf));
                 }
                 if(send_fd) close(send_fd);
            }
        }
        else if(strstr(cmd_buf,"send-dir-req:")!=NULL)
        {
            int s_flag = 0;
            int i =0;
            int rv = 0;
            int sum = 0;
            int s_len = 0;
            int file_index = 0;
            char buff[4]={0};
            char** files_local = NULL;
            char** files_remote = NULL;
            dir_name = cmd_buf+SEND_DIR_LEN;
            printf("recv dir_name is %s\n",dir_name);

            while (1)
            {
                //printf("recv buf:%s \n", s_buf);
                if (s_flag == 0)
                {
                    s_flag = 1;
                    memset(buff, 0, 4);
                    rv=read(fd,buff,sizeof(buff));
                    if (rv <= 0)
                    {
                        printf("recv failed(file list). %d\n", rv);
                        break;
                    }
                    memcpy(&s_total_filecount, buff, 4);
                    printf("file_count:%d \n", s_total_filecount);
                    files_local = malloc(s_total_filecount * sizeof(char*));
                    files_remote = malloc(s_total_filecount * sizeof(char*));
                    for (i = 0; i < s_total_filecount; i++)
                    {
                        files_local[i] = malloc(MAX_PATH_LENGTH);
                        files_remote[i] = malloc(MAX_PATH_LENGTH);
                        memset(files_local[i], 0, MAX_PATH_LENGTH);
                        memset(files_remote[i], 0, MAX_PATH_LENGTH);
                    }
                }
                else
                {
					memset(buff, 0, 4);
					memset(file_buf, 0, sizeof(file_buf));
					rv=read(fd,buff,sizeof(buff));
					if (rv <= 0)
					{
						printf("recv failed(file list). %d %d\n", rv, s_len);
						break;
					}
					memcpy(&s_len_filename, buff, 4);
					printf("s_len_filename:%d\n",s_len_filename);
					printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);

					rv=read(fd,file_buf,s_len_filename);
                    if (rv <= 0)
                    {
                        printf("recv failed(file list). %d %d\n", rv, s_len);
                        break;
                    }
                    //printf("file_path:%s \n", s_buf);

                    strcpy(files_local[file_index], dir_name);
                    strcat(files_local[file_index], file_buf);
                    //strcpy(files_remote[file_index], thread_param.remote_path);
                    strcat(files_remote[file_index], file_buf);
                    printf("local %d :%s,remote:%s\n",file_index,files_local[file_index],files_remote[file_index]);
                    file_index++;
                }
                sum += rv;

                if (file_index >= s_total_filecount)
                {
                    printf("total recv:%d,file_index:%d \n\n", sum,file_index);
                    break;
                }
            }

            s_flag = 0;
            sum = 0;
            char dir_check[256];
            for (i = 0; i < s_total_filecount; i++)
            {
				memset(dir_check, 0, 256);
				strncpy(dir_check, files_local[i], strlen(files_local[i]) - strlen(strrchr(files_local[i], '/')));
				if (access(dir_check, F_OK) != 0)
				{
					create_dir1(dir_check);
				}

				//printf("open %s!\n", files_remote[i]);
				s_fp = fopen(files_local[i], "wb+");
				if (s_fp == NULL) {
					printf("\033[0m\033[41;33mopen %s failed!\033[0m\n", files_local[i]);
					continue;
				}

				//printf("recv %s!\n", files_remote[i]);
				while (1)
				{
					if (s_flag == 0)
					{
						s_flag = 1;
						unsigned char buff[4];
						memset(buff, 0, 4);
						rv=read(fd,buff,sizeof(buff));
						if (rv <= 0)
						{
							//printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
							continue;
						}
						//s_total_len = *((int*)s_buf);
						memcpy(&s_total_len, buff, 4);
						if (s_total_len == 0) break;
						file = (char*)malloc(s_total_len);
						printf("total len: %d.\n", s_total_len);
						//printf("start: %x %x %x %x\n", buff[0], buff[1], buff[2], buff[3]);
					}
					else
					{
						rv=read(fd,file_buf,sizeof(file_buf));
						if (rv <= 0)
						{
							printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
							continue;
						}

						memcpy(file+sum-4, file_buf, rv);
						//printf("sum:%d ", sum);
						//fwrite(s_buf, rv, 1, s_fp);
					}
					sum += rv;
					printf(".");
					if (sum >= s_total_len + 4)
					{
						fwrite(file, 1, s_total_len, s_fp);
						printf("\n>>>> download %d file: %s ok : %d.<<<<<\n\n",i,files_remote[i], sum);
						break;
					}
				}
				if (file != NULL) free(file);
				file = NULL;
				sum = 0;
				s_flag = 0;
				s_total_len = 0;
				if (s_fp!=NULL) fclose(s_fp);
            }

            if (s_total_filecount > 0)
            {
				for (i = 0; i < s_total_filecount; i++)
				{
					free(files_local[i]);
					free(files_remote[i]);
				}
				free(files_local);
				free(files_remote);
            }
            s_total_filecount = 0;
        }
    }
    return 0;
}

void *send_rndis_file_loop(void *arg)
{
    int rc;
    unsigned char buff4[4] ={0};

    while(1)
    {
        int link_id = accept(sockfd, (struct sockaddr *)&clie_addr, &clie_addr_len);
        if (-1 == link_id) {
            perror("Accept socket failed\n");
            return;
        }
        printf("IP is %d\n",inet_ntoa(clie_addr.sin_addr));
        printf("Port is %d\n",ntohs(clie_addr.sin_port));
        printf("%d\n",(sockfd));
        memset(cmd_buf, 0, CMD_MAX_LEN);
        rc = recv(link_id, cmd_buf, CMD_MAX_LEN, 0);//鎺ユ敹鏉ヨ嚜client鐨勬暟鎹紝瀹㈡埛绔涓€娆¤鍙戦€佺殑鏂囦欢鍚嶇О
        if (rc <= 0)
        {
            printf("recv failed1 %s\n", strerror(errno));
        }
        if(strstr(cmd_buf,"read-req:")!=NULL)
        {

            send_name = cmd_buf+READ_LEN;
            printf("send name is %s\n",send_name);

            send_fd = open(send_name,O_RDONLY);
            if(send_fd == -1)
            {
                printf("open failed\n");
                return;
            }

			send_len= lseek(send_fd,0L,SEEK_END);
			lseek(send_fd,0L,SEEK_SET);
			printf("send file size is %d\n",send_len);

			memcpy(buff4,&send_len,sizeof(buff4));
			printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
			rc = send(link_id, buff4, 4, 0);
			if(rc != 4)
			{
			printf("write length failed ret is %d\n",rc);
			return;
			}
            //sleep(1);
            total_len = send_len;
            while(1)
            {
                rc= read(send_fd, file_buf, sizeof(file_buf));
                //printf("read length ret is %d\n",ret);
                if(rc <= -1) {
                    printf("read Error\n");
                    break;
                } else if(rc == 0) {
                    printf("read finished\n");
                    break;
                }

				send(link_id, file_buf, rc, 0);
				send_len-=rc;

                //printf("left length ret is %d\n",send_len);
                if(send_len <= 0) {
                    printf("sync!\n");
                    break;
    	        }
                memset(file_buf,0,sizeof(file_buf));
			}
			if(send_fd) close(send_fd);
        }
        else if(strstr(cmd_buf,"send-req:")!=NULL)
        {
            store_name = cmd_buf+WRITE_LEN;
            printf("store name is %s\n",store_name);
            store_fd = open(store_name,O_WRONLY | O_CREAT |O_TRUNC);
            if(store_fd == -1)
            {
                printf("store file open failed\n");
                return;
            }

            while(1)
            {
                if( !got_store_len )
                {
                    unsigned char buff[4];
                    memset(buff, 0, 4);
                    rc = recv(link_id, buff, sizeof(buff), 0);
                    if (rc <= 0)
                    {
                        printf("recv failed2 %s\n", strerror(errno));
                    }

                    memcpy(&store_len,buff,sizeof(buff));
                    total_len = store_len;
                    //start_time = time(NULL);
                    //printf("store_len is %d, start time is %d\n",store_len,start_time);
                    printf("store_len is %d,rc:%d\n",store_len,rc);
    		        printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);
                    got_store_len = true;
                }
                rc = recv(link_id, file_buf, sizeof(file_buf), 0);
                if (rc < 0)
                {
                    printf("recv failed3 %s\n", strerror(errno));
                }
                else if(rc == 0)
                {
                    printf("read finished\n");
                    break;
                }

                write(store_fd, file_buf, rc);

                store_len -= rc;
                // printf("left length ret is %d\n",store_len);
                if(store_len <= 0)
                {
                    printf("store_len is empty\n");
                    break;
                }

                memset(file_buf,0,sizeof(file_buf));
            }
            if(store_fd) close(store_fd);
        }
        else if(strstr(cmd_buf,"read-dir-req:")!=NULL)
        {
            unsigned long len = 0;
            dir_name = cmd_buf+READ_DIR_LEN;
            printf("dir_name is %s\n",dir_name);
            //dir_name[strlen(dir_name)]='\0';
            if((file_list_count=  get_file_count(dir_name))==-1) return;
			
            total = 0;
            memcpy(buff4,&file_list_count,sizeof(buff4));
            printf("file_list_count is %d\n",file_list_count);

            //write(fd, buff4, sizeof(buff4));
            rc = send(link_id, buff4, 4, 0);
            int i=0;
     	    usleep(10000*file_list_count);
            for(i = 0; i < file_list_count; ++i)
            {
                memset(cmd_buf,0,sizeof(cmd_buf));
                if(strlen(files_path[i]) >= sizeof(cmd_buf))
                {
                    printf("file name lenth too long %d\n", strlen(files_path[i]));
                    return;
                }
				
                strcpy(cmd_buf,files_path[i]);
                //send length of file name
                len = strlen(cmd_buf);
                memcpy(buff4,&len,sizeof(buff4));
                printf("wk:%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
                send(link_id, buff4, sizeof(buff4), 0);

                printf("path ot send :%s,%d\n",cmd_buf,strlen(cmd_buf));
                send(link_id, cmd_buf, strlen(cmd_buf), 0);
            }

            for(i=0;i<file_list_count;++i)
            {
                send_fd = open(files_path[i],O_RDONLY);
                if(send_fd == -1)
                {
                    printf("open failed\n");
                    return;
                }
                printf("open file %s\n",files_path[i]);

                send_len= lseek(send_fd,0L,SEEK_END);
                lseek(send_fd,0L,SEEK_SET);
                printf("send file size is %d\n",send_len);

                memcpy(buff4,&send_len,sizeof(buff4));
				printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
				sleep(1);
                rc = send(link_id, buff4, 4, 0);
                if(rc != 4)
                {
                    printf("write length failed ret is %d\n",rc);
                    return;
                }

                usleep(10000);
                total_len = send_len;
                while(1)
                {
					//usleep(100000);
                    rc= read(send_fd, file_buf, sizeof(file_buf));
                    //printf("read length ret is %d\n",ret);
                    if(rc <= -1) {
                        printf("read Error\n");
                        break;
                    } else if(rc == 0) {
                        printf("read finished\n");
                        break;
                    }

					send(link_id, file_buf, rc, 0);
					send_len-=rc;

                    //printf("left length ret is %d\n",send_len);
                    if(send_len <= 0) {
                        printf("sync!\n");
                        break;
        	        }
                    memset(file_buf,0,sizeof(file_buf));
                }
                if(send_fd) close(send_fd);
            }
        }
        else if(strstr(cmd_buf,"send-dir-req:")!=NULL)
        {
            int s_flag = 0;
            int i =0;
            int rv = 0;
            int sum = 0;
            int s_len = 0;
            int file_index = 0;
            char buff[4]={0};
            char** files_local = NULL;
            char** files_remote = NULL;
            dir_name = cmd_buf+SEND_DIR_LEN;
            printf("recv dir_name is %s\n",dir_name);

            while (1)
            {
                //printf("recv buf:%s \n", s_buf);
                if (s_flag == 0)
                {
                    s_flag = 1;
                    memset(buff, 0, 4);
                    rv = recv(link_id, buff, sizeof(buff), 0);
                    if (rv <= 0)
                    {
                        printf("recv failed(file list). %d\n", rv);
                        break;
                    }
                    memcpy(&s_total_filecount, buff, 4);
                    printf("file_count:%d \n", s_total_filecount);
                    files_local = malloc(s_total_filecount * sizeof(char*));
                    files_remote = malloc(s_total_filecount * sizeof(char*));
                    for (i = 0; i < s_total_filecount; i++)
                    {
                        files_local[i] = malloc(MAX_PATH_LENGTH);
                        files_remote[i] = malloc(MAX_PATH_LENGTH);
                        memset(files_local[i], 0, MAX_PATH_LENGTH);
                        memset(files_remote[i], 0, MAX_PATH_LENGTH);
                    }
                }
                else
                {
                      memset(buff, 0, 4);
                      memset(file_buf, 0, sizeof(file_buf));
                      rv = recv(link_id, buff, sizeof(buff), 0);
                		if (rv <= 0)
                    	{
                    		printf("recv failed(file list). %d %d\n", rv, s_len);
                    		break;
                    	}
                    	memcpy(&s_len_filename, buff, 4);
                     printf("s_len_filename:%d\n",s_len_filename);
                     printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);

                     rv = recv(link_id, file_buf, s_len_filename, 0);
                    if (rv <= 0)
                    {
                        printf("recv failed(file list). %d %d\n", rv, s_len);
                        break;
                    }
                    //printf("file_path:%s \n", s_buf);

                    strcpy(files_local[file_index], dir_name);
                    strcat(files_local[file_index], file_buf);
                    //strcpy(files_remote[file_index], thread_param.remote_path);
                    strcat(files_remote[file_index], file_buf);
                    printf("local %d :%s,remote:%s\n",file_index,files_local[file_index],files_remote[file_index]);
                    file_index++;
                }
                sum += rv;

                if (file_index >= s_total_filecount)
                {
                    printf("total recv:%d,file_index:%d \n\n", sum,file_index);
                    break;
                }
            }

            s_flag = 0;
            sum = 0;
            char dir_check[256];
            for (i = 0; i < s_total_filecount; i++)
            {
				memset(dir_check, 0, 256);
				strncpy(dir_check, files_local[i], strlen(files_local[i]) - strlen(strrchr(files_local[i], '/')));
				if (access(dir_check, F_OK) != 0)
				{
					create_dir1(dir_check);
				}

				//printf("open %s!\n", files_remote[i]);
				s_fp = fopen(files_local[i], "wb+");
				if (s_fp == NULL) {
					printf("\033[0m\033[41;33mopen %s failed!\033[0m\n", files_local[i]);
					continue;
				}

				//printf("recv %s!\n", files_remote[i]);
				while (1)
				{
					if (s_flag == 0)
					{
						s_flag = 1;
						unsigned char buff[4];
						memset(buff, 0, 4);
						rv = recv(link_id, buff, sizeof(buff), 0);
						if (rv <= 0)
						{
							//printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
							continue;
						}
						//s_total_len = *((int*)s_buf);
						memcpy(&s_total_len, buff, 4);
						if (s_total_len == 0) break;
						file = (char*)malloc(s_total_len);
						printf("total len: %d.\n", s_total_len);
						//printf("start: %x %x %x %x\n", buff[0], buff[1], buff[2], buff[3]);
					}
					else
					{
						rv = recv(link_id, file_buf, sizeof(file_buf), 0);
						if (rv <= 0)
						{
							printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
							continue;
						}

						memcpy(file+sum-4, file_buf, rv);
						//printf("sum:%d ", sum);
						//fwrite(s_buf, rv, 1, s_fp);
					}
					sum += rv;
					printf(".");
					if (sum >= s_total_len + 4)
					{
						fwrite(file, 1, s_total_len, s_fp);
						printf("\n>>>> download %d file: %s ok : %d.<<<<<\n\n",i,files_remote[i], sum);
						break;
					}
				}
				
				if (file != NULL) free(file);
				file = NULL;
				sum = 0;
				s_flag = 0;
				s_total_len = 0;
				if (s_fp!=NULL) fclose(s_fp);
            }

            if (s_total_filecount > 0)
            {
				for (i = 0; i < s_total_filecount; i++)
				{
					free(files_local[i]);
					free(files_remote[i]);
				}
				free(files_local);
				free(files_remote);
            }
            s_total_filecount = 0;
        }
    }
}

int main(int argc, char *argv[])
{
    int r;
    int err = 0;

    sockfd = socket(AF_INET,SOCK_STREAM,0); /*create a socket*/
    /*init servaddr*/
    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERV_PORT);
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    /*bind address and port to socket*/
    if(bind(sockfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) == -1)
    {
        perror("bind error");
        exit(-1);
    }

    if (-1 == listen(sockfd, 10)) {
        perror("Listen socket failed\n");
        exit(0);
    }
    printf("------------listen to client-------------\n");
    r = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, r & ~O_NONBLOCK);

    int i = 0;
    char dev[MAX_PATH_LENGTH]={0};
    handle = DataBufferCreate(10240);

    for(i=0;i<=4;i++){
        snprintf(dev,MAX_PATH_LENGTH,"/dev/ttyACM%d",i);
        fd = OpenDev(dev);
        if (fd>0){
            printf("To open Serial Port ttyACM%d success\n",i);
            break;
        }
    }
    if (fd < 0)
    {
        printf("sdssgl Can't Open any Serial Port\n");
        exit(0);
    }
    set_speed(fd,115200);
    if (set_Parity(fd,8,1,'N')== false)
    {
        printf("sdssgl Set Parity Error\n");
        exit(1);
    }

    err = pthread_create(&rndis_tid,NULL,send_rndis_file_loop,NULL);
    if(err != 0){
        printf("can't create rndis thread: %s\n",strerror(err));
        return -1;
    }
    err = pthread_create(&vcom_tid,NULL,send_vcom_file_loop,NULL);
    if(err != 0){
        printf("can't create vcom thread: %s\n",strerror(err));
        return -1;
    }

    while(1);

    close(sockfd);
	DataBufferDestroy(handle);
    close(fd);
    printf("scok and vcom finish\n");
    return 0;

}
