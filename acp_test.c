#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<fcntl.h>
#include<sys/stat.h>
#include<errno.h>
#include<sys/select.h>
#include<sys/time.h>
#include <unistd.h>

#define MAX_SIZE        	1024*40
#define SERV_PORT       	5000
#define PACKET_SIZE     	2048
#define MAX_PATH_LENGTH		128
#define FILE_NUM		  	3

struct thread_param_t
{
	rt_bool_t is_file_opt;
	rt_bool_t is_pull_opt;
	char local_path[MAX_PATH_LENGTH];
	char remote_path[MAX_PATH_LENGTH];
};

static rt_device_t serial = NULL;
static FILE *s_fp = NULL;
static int s_flag = 0;	// 用来标记收到文件的第一帧，表示文件长度
static int s_total_len = 0;
static int s_len = 0;
static char s_buf[20480];
static rt_sem_t sem_thread_exit = NULL;
static rt_sem_t tx_sem = NULL;
static rt_sem_t sem_file_send_ok = NULL;
static rt_mq_t rx_mq_p = NULL;

static struct thread_param_t thread_param;
static int s_total_filecount = 0;
static int s_len_filename = 0;

static int send_fd = -1;
static int send_len = 0;
static int total_len = 0;
static int sockfd;
char file_buf[PACKET_SIZE] = {0};
struct sockaddr_in servaddr;
char files_path[100][MAX_PATH_LENGTH]={0};
static int file_list_count = 0;
static int total = 0;
static unsigned char buff4[4] ={0};
static char cmd_buf[MAX_PATH_LENGTH] = {0};
static int tool_flag = 0;


/* 串口接收消息结构*/
struct rx_msg
{
    rt_size_t size;
};


static rt_err_t uart_input(rt_device_t dev, rt_size_t size)
{
	if (rx_mq_p == NULL) return RT_EOK;
	s_len = size;
	struct rx_msg msg;
	rt_err_t result;
	msg.size = size;
	result = rt_mq_send( rx_mq_p, &msg, sizeof(msg));
	if ( result == -RT_EFULL)
	{
		printf("message queue full\n");
	}
	return RT_EOK;
}

static rt_err_t uart_output(rt_device_t dev, void *buffer)
{
	if (tx_sem == NULL) return RT_EOK;
	rt_sem_release(tx_sem);

    return sizeof(buffer);
}

static int get_file_and_count(const char* basePath)
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
	while((ptr = readdir(dir)) != NULL)
	{
		printf("ptr->d_type:%d,%s\n",ptr->d_type,ptr->d_name);
		//顺序读取每一个目录项；
		//跳过“…”和“.”两个目录
		if(strcmp(ptr->d_name,".") == 0 || strcmp(ptr->d_name,"..") == 0)
		{
			continue;
		}

		//如果是目录，则递归调用 get_file_count函数
		if(ptr->d_type == 2)
		{
			memset(path,0,sizeof(path));
			sprintf(path,"%s/%s",basePath,ptr->d_name);

			printf("dir is %s\n",path);
			get_file_and_count(path);
		}

		if(ptr->d_type == 1)
		{
			memset(path,0,sizeof(path));
			sprintf(path,"%s/%s",basePath,ptr->d_name);
			strcpy(files_path[total++],path);
			printf("path %d is %s\n",total,path);
		}
	}

	if(errno != 0)
	{
		printf("fail to read dir:%d\n",errno); //失败则输出提示信息
		return -1;
	}
	closedir(dir);
	return total;
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

	return 0;
}

int connect_to_server()
{
    int r;
    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family= AF_INET;
    servaddr.sin_port = htons(SERV_PORT);
    servaddr.sin_addr.s_addr = inet_addr("192.168.1.36");
    sockfd =socket(AF_INET,SOCK_STREAM,0);

    r = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, r & ~O_NONBLOCK);

    /* 连接到服务端 */
    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(struct sockaddr)) == -1)
    {
        /* 连接失败 */
        printf("Connect fail!\n");
        rt_sem_release(sem_thread_exit);
        return -1;
    }

    return 0;
}

static void _thread_recv_dir(void *parameter)
{
    int rv;
    char buf[MAX_PATH_LENGTH];
    int sum = 0;
    char *file=NULL;
    char** files_local = NULL;
    char** files_remote = NULL;
    unsigned char buff[4];

    if(-1==connect_to_server()){
        return;
    }

	// read remote directory file list
	if (!thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		sprintf(buf, "read-dir-req:%s", thread_param.remote_path);
         printf("dir:%s\n",buf);
         send(sockfd, buf, MAX_PATH_LENGTH, 0);
		int file_index = 0;

		while (1)
		{
			//printf("recv buf:%s \n", s_buf);
			if (s_flag == 0)
			{
				s_flag = 1;
				memset(buff, 0, 4);
				rv = recv(sockfd, buff, sizeof(buff), 0);
				if (rv <= 0)
				{
					printf("recv failed(file list). %d %d\n", rv, s_len);
					break;
				}
				memcpy(&s_total_filecount, buff, 4);
				printf("file_count:%d \n", s_total_filecount);
				files_local = malloc(s_total_filecount * sizeof(char*));
				files_remote = malloc(s_total_filecount * sizeof(char*));
				RT_ASSERT(files_local != NULL);
				RT_ASSERT(files_remote != NULL);
				for (size_t i = 0; i < s_total_filecount; i++)
				{
					files_local[i] = malloc(MAX_PATH_LENGTH);
					files_remote[i] = malloc(MAX_PATH_LENGTH);
					RT_ASSERT(files_local[i] != NULL);
					RT_ASSERT(files_remote[i] != NULL);
					memset(files_local[i], 0, MAX_PATH_LENGTH);
					memset(files_remote[i], 0, MAX_PATH_LENGTH);
				}
			}
			else
			{
				memset(buff, 0, 4);
				memset(file_buf, 0, sizeof(file_buf));
				rv = recv(sockfd, buff, sizeof(buff), 0);
				if (rv <= 0)
				{
					printf("recv failed(file list). %d %d\n", rv, s_len);
					break;
				}
				memcpy(&s_len_filename, buff, 4);
				printf("s_len_filename:%d\n",s_len_filename);
				printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);

				rv = recv(sockfd, file_buf, s_len_filename, 0);
				if (rv <= 0)
				{
					printf("recv failed(file list). %d %d\n", rv, s_len);
					break;
				}

				//printf("file_path:%s \n", s_buf);
				strcpy(files_local[file_index], thread_param.local_path);
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
	}

	s_flag = 0;
	sum = 0;
	char dir_check[256];
	for (int i = 0; i < s_total_filecount; i++)
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

		//rt_sem_take(tx_sem, RT_WAITING_FOREVER);
		//printf("recv %s!\n", files_remote[i]);
		while (1)
		{
			//rv = rt_device_read(serial, 0, s_buf, rx_msg.size);

			if (s_flag == 0)
			{
				s_flag = 1;
				memset(buff, 0, 4);
				rv = recv(sockfd, buff, sizeof(buff), 0);
				if (rv <= 0)
				{
					//printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
					continue;
				}
				//s_total_len = *((int*)s_buf);
				memcpy(&s_total_len, buff, 4);
				if (s_total_len == 0) break;
				file = (char*)malloc(s_total_len);
				printf("save to %s,total len: %d.\n", files_local[i],s_total_len);
				//printf("start: %x %x %x %x\n", buff[0], buff[1], buff[2], buff[3]);
			}
			else
			{
				rv = recv(sockfd, file_buf, sizeof(file_buf), 0);
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
		for (size_t i = 0; i < s_total_filecount; i++)
		{
			free(files_local[i]);
			free(files_remote[i]);
		}
		free(files_local);
		free(files_remote);
	}

	s_total_filecount = 0;
	rt_sem_release(sem_thread_exit);
}

static void _thread_recv(void *parameter)
{
    int rv;
    char buf[MAX_PATH_LENGTH];
    int sum = 0;
    char *file=NULL;
    char** files_local = NULL;
    char** files_remote = NULL;


    if(-1==connect_to_server()){
        return;
    }
    // read remote directory file list
	if (!thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		sprintf(buf, "read-dir-req:%s", thread_param.remote_path);
        send(sockfd, buf, MAX_PATH_LENGTH, 0);
		int file_index = 0;

		while (1)
		{
			rv = recv(sockfd, file_buf, sizeof(file_buf), 0);
			if (rv <= 0)
			{
				printf("recv failed(file list). %d %d\n", rv, s_len);
				break;
			}

			if (s_flag == 0)
			{
				s_flag = 1;
				memcpy(&s_total_filecount, file_buf, 4);
				printf("file_count:%d \n", s_total_filecount);
				files_local = malloc(s_total_filecount * sizeof(char*));
				files_remote = malloc(s_total_filecount * sizeof(char*));
				RT_ASSERT(files_local != NULL);
				RT_ASSERT(files_remote != NULL);
				for (size_t i = 0; i < s_total_filecount; i++)
				{
					files_local[i] = malloc(MAX_PATH_LENGTH);
					files_remote[i] = malloc(MAX_PATH_LENGTH);
					RT_ASSERT(files_local[i] != NULL);
					RT_ASSERT(files_remote[i] != NULL);
					memset(files_local[i], 0, MAX_PATH_LENGTH);
					memset(files_remote[i], 0, MAX_PATH_LENGTH);
				}
			}
			else
			{
				//printf("file_path:%s \n", s_buf);
				strcpy(files_local[file_index], thread_param.local_path);
				strcat(files_local[file_index], file_buf);
				strcpy(files_remote[file_index], thread_param.remote_path);
				strcat(files_remote[file_index], file_buf);
				file_index++;
			}

			sum += rv;
			if (file_index >= s_total_filecount)
			{
				printf("total recv:%d \n", sum);
				break;
			}
		}
	}
	else if (thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		s_total_filecount = 1;
        printf("s_total_filecount is:%d \n", s_total_filecount);

		files_local = malloc(s_total_filecount * sizeof(char*));
		files_remote = malloc(s_total_filecount * sizeof(char*));
		files_local[0] = malloc(MAX_PATH_LENGTH);
		files_remote[0] = malloc(MAX_PATH_LENGTH);
		RT_ASSERT(files_local[0] != NULL);
		RT_ASSERT(files_remote[0] != NULL);
		memset(files_local[0], 0, MAX_PATH_LENGTH);
		memset(files_remote[0], 0, MAX_PATH_LENGTH);
		strcpy(files_remote[0], thread_param.remote_path);
		strcpy(files_local[0], thread_param.local_path);
	}

	s_flag = 0;
	sum = 0;
	char dir_check[256];
	for (int i = 0; i < s_total_filecount; i++)
	{
		memset(dir_check, 0, 256);
		strncpy(dir_check, files_local[i], strlen(files_local[i]) - strlen(strrchr(files_local[i], '/')));
		if (access(dir_check, F_OK) != 0)
		{
			create_dir1(dir_check);
		}
		s_fp = fopen(files_local[i], "wb+");
		if (s_fp == NULL) {
			printf("\033[0m\033[41;33mopen %s failed!\033[0m\n", files_local[i]);
			continue;
		}
        sprintf(buf, "read-req:%s", files_remote[i]);
        send(sockfd, buf, MAX_PATH_LENGTH, 0);

		printf("\nrecv %s!\n", files_remote[i]);
		while (1)
		{
			if (s_flag == 0)
			{
				printf("start to recv length!\n");
				s_flag = 1;
				unsigned char buff[4];
				memset(buff, 0, 4);
				rv = recv(sockfd, buff, sizeof(buff), 0);
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
				printf("start: %x %x %x %x\n", buff[0], buff[1], buff[2], buff[3]);
			}
			else
			{
				rv = recv(sockfd, file_buf, sizeof(file_buf), 0);
				if (rv <= 0)
				{
					//printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
					continue;
				}

				memcpy(file+sum-4, file_buf, rv);
				//printf("sum:%d ", sum);
				//fwrite(s_buf, rv, 1, s_fp);
			}
			sum += rv;

			if (sum >= s_total_len + 4)
			{
				fwrite(file, 1, s_total_len, s_fp);
				printf("download ok: %d.\n", sum);
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
		for (size_t i = 0; i < s_total_filecount; i++)
		{
			free(files_local[i]);
			free(files_remote[i]);
		}
		free(files_local);
		free(files_remote);
	}
	s_total_filecount = 0;
	rt_sem_release(sem_thread_exit);
}

static void _thread_send_dir(void *parameter)
{
    char buf[MAX_PATH_LENGTH];
    struct stat s;
    int rc = 0;
    int len =0;

	//cmd
    if (-1==connect_to_server()){
        return;
    }

	stat(thread_param.local_path, &s);
	if (S_ISDIR(s.st_mode) == 1) {	// 如果要发送的是目录，则依次发送目录下每个文件
	    sprintf(buf, "send-dir-req:%s", thread_param.remote_path);
        printf("send dir:%s\n",buf);
        send(sockfd, buf, MAX_PATH_LENGTH, 0);
        if((file_list_count = get_file_and_count(thread_param.local_path))== -1){
            rt_sem_release(sem_thread_exit);
            return;
        }

        total = 0;
        memcpy(buff4,&file_list_count,sizeof(buff4));
        printf("file_list_count is %d\n",file_list_count);

        //write(fd, buff4, sizeof(buff4));
        rc = send(sockfd, buff4, 4, 0);
        int i=0;
        //usleep(10000*file_list_count);
        for(i=0;i<file_list_count;++i)
        {
            memset(cmd_buf,0,sizeof(cmd_buf));
            if(strlen(files_path[i]) >= sizeof(cmd_buf))
            {
				printf("file name lenth too long %d\n", strlen(files_path[i]));
				rt_sem_release(sem_thread_exit);
				return;
            }
            strcpy(cmd_buf,files_path[i]);

            //send length of file name
            len = strlen(cmd_buf);
            memcpy(buff4,&len,sizeof(buff4));
            printf("wk:%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
            send(sockfd, buff4, sizeof(buff4), 0);
            //usleep(10000*file_list_count);

            //int err = write(fd, cmd_buf, sizeof(cmd_buf));
            printf("path ot send :%s,%d\n",cmd_buf,strlen(cmd_buf));
            send(sockfd, cmd_buf, strlen(cmd_buf), 0);
        }
	} else if (S_ISREG(s.st_mode) == 1) {
		//open
		s_fp = fopen(thread_param.local_path, "rb");
		if (s_fp == NULL) {
			printf("fopen %s failed!\n",thread_param.local_path);
			rt_sem_release(sem_thread_exit);
			return;
		}
		printf("fopen %s ok!\n", thread_param.local_path);

		memset(buf, 0, sizeof(buf));
		sprintf(buf, "send-req:%s", thread_param.remote_path);
    }

    for(int i=0;i<file_list_count;++i)
    {
        send_fd = open(files_path[i],O_RDONLY);
        if(send_fd == -1)
        {
            printf("open failed\n");
            break;
        }

        printf("open file %s\n",files_path[i]);
        send_len= lseek(send_fd,0L,SEEK_END);
        lseek(send_fd,0L,SEEK_SET);
        printf("send file size is %d\n",send_len);

        memcpy(buff4,&send_len,sizeof(buff4));
        printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
        sleep(1);
        rc = send(sockfd, buff4, 4, 0);
        if(rc != 4)
        {
            printf("write length failed ret is %d\n",rc);
            break;
        }

        //usleep(10000);

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

            send(sockfd, file_buf, rc, 0);
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

    rt_sem_release(sem_thread_exit);
}

static void _thread_send(void *parameter)
{
    int sum = 0;
    int filelen = 0;
    char *file=NULL;
    char buf[MAX_PATH_LENGTH];

	//open
	s_fp = fopen(thread_param.local_path, "rb");
	if (s_fp == NULL) {
		printf("fopen %s failed!\n",thread_param.local_path);
		rt_sem_release(sem_thread_exit);
		return;
	}

	printf("fopen %s ok!\n", thread_param.local_path);
	memset(buf, 0, sizeof(buf));
	sprintf(buf, "send-req:%s", thread_param.remote_path);

	//cmd
    if(-1==connect_to_server()){
        return;
    }

	printf("send %s.!\n", buf);
    send(sockfd, buf, MAX_PATH_LENGTH, 0);
    while (1)
    {
		if (s_flag == 0)
		{
			char bin[4];
			memset(bin,0,4);

			s_flag = 1;
			fseek(s_fp, 0, SEEK_END);
			filelen = ftell(s_fp);
			file = (char*)malloc(filelen);
			fseek(s_fp, 0, SEEK_SET);
			fread(file, filelen, 1, s_fp);
			memcpy(bin, &filelen, 4);
			int store_len=0;
			memcpy(&store_len,bin,sizeof(bin));

			printf("store_len is %d\n",store_len);
			printf("%02x %02x %02x %02x\n", bin[0], bin[1], bin[2], bin[3]);

			send(sockfd, bin, 4, 0);
		}
		else
		{
			if (filelen - sum > PACKET_SIZE)
			{
				send(sockfd, file+sum, PACKET_SIZE, 0);
				sum += PACKET_SIZE;
			}
			else
			{
				printf("+++++send last packet . %d.\n", filelen);
				send(sockfd, file+sum, filelen - sum, 0);
				sum = 0;
				s_flag = 0;
				free(file);
				file = NULL;
				if (s_fp!=NULL) {
					fclose(s_fp);
					s_fp = NULL;
					printf("close the send file\n");
				}
				break;
			}
		}
    }

    rt_sem_release(sem_thread_exit);
}

static void _thread_vcom_send_dir(void *parameter)
{
    char buf[MAX_PATH_LENGTH];
    struct stat s;
    int rc = 0;
    int len =0;

	stat(thread_param.local_path, &s);
	if (S_ISDIR(s.st_mode) == 1) {	// 如果要发送的是目录，则依次发送目录下每个文件
		sprintf(buf, "send-dir-req:%s", thread_param.remote_path);
		printf("send dir:%s\n",buf);
		//send(sockfd, buf, MAX_PATH_LENGTH, 0);
		rt_device_write(serial, 0, buf, MAX_PATH_LENGTH);
		rt_sem_take(tx_sem, RT_WAITING_FOREVER);

		if((file_list_count = get_file_and_count(thread_param.local_path))== -1){
			rt_sem_release(sem_thread_exit);
			return;
		}
        total = 0;
        memcpy(buff4,&file_list_count,sizeof(buff4));
        printf("file_list_count is %d\n",file_list_count);

        rt_device_write(serial, 0,buff4, sizeof(buff4));
        rt_sem_take(tx_sem, RT_WAITING_FOREVER);

        //rc = send(sockfd, buff4, 4, 0);
        int i=0;
        //usleep(10000*file_list_count);
        for(i=0;i<file_list_count;++i)
        {
            memset(cmd_buf,0,sizeof(cmd_buf));
            if(strlen(files_path[i]) >= sizeof(cmd_buf))
            {
                 printf("file name lenth too long %d\n", strlen(files_path[i]));
                 rt_sem_release(sem_thread_exit);

                 return;
            }
            strcpy(cmd_buf,files_path[i]);

            //send length of file name
            len = strlen(cmd_buf);
            memcpy(buff4,&len,sizeof(buff4));
            printf("wk:%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);
            rt_device_write(serial, 0,buff4, sizeof(buff4));
            rt_sem_take(tx_sem, RT_WAITING_FOREVER);

            printf("path ot send :%s,%d\n",cmd_buf,strlen(cmd_buf));
            rt_device_write(serial, 0,cmd_buf, strlen(cmd_buf));
            rt_sem_take(tx_sem, RT_WAITING_FOREVER);
        }
	} else if (S_ISREG(s.st_mode) == 1) {
        s_fp = fopen(thread_param.local_path, "rb");
        if (s_fp == NULL) {
			printf("fopen %s failed!\n",thread_param.local_path);
			rt_sem_release(sem_thread_exit);
			return;
        }

        printf("fopen %s ok!\n", thread_param.local_path);
        memset(buf, 0, sizeof(buf));
        sprintf(buf, "send-req:%s", thread_param.remote_path);
    }

    for(int i=0;i<file_list_count;++i)
    {
        send_fd = open(files_path[i],O_RDONLY);
        if(send_fd == -1)
        {
            printf("open failed\n");
            break;
        }

        printf("open file %s\n",files_path[i]);
        send_len= lseek(send_fd,0L,SEEK_END);
        lseek(send_fd,0L,SEEK_SET);
        printf("send file size is %d\n",send_len);

        memcpy(buff4,&send_len,sizeof(buff4));
        printf("%02x %02x %02x %02x\n", buff4[0], buff4[1], buff4[2], buff4[3]);

        rt_device_write(serial, 0,buff4, sizeof(buff4));
        rt_sem_take(tx_sem, RT_WAITING_FOREVER);

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

            rt_device_write(serial, 0,file_buf, rc);
            rt_sem_take(tx_sem, RT_WAITING_FOREVER);

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

    rt_sem_release(sem_thread_exit);
}

static void _thread_vcom_recv_dir(void *parameter)
{
	int rv;
	char buf[MAX_PATH_LENGTH];
	int sum = 0;
	char *file=NULL;
	char** files_local = NULL;
	char** files_remote = NULL;
	struct rx_msg rx_msg;
	rt_err_t result;
	//unsigned char buff[4];

	// read remote directory file list
	if (!thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		sprintf(buf, "read-dir-req:%s", thread_param.remote_path);
		printf("dir:%s\n",buf);
		rt_device_write(serial, 0, buf, MAX_PATH_LENGTH);
		rt_sem_take(tx_sem, RT_WAITING_FOREVER);
		int file_index = 0;

		while (1)
		{
			rt_memset(&rx_msg, 0, sizeof(rx_msg));
			result = rt_mq_recv(rx_mq_p, &rx_msg, sizeof(rx_msg), 300);
			rv = rt_device_read(serial, 0, s_buf, rx_msg.size);
			if (rv <= 0)
			{
				printf("_thread_vcom_recv_dir read buff 4 failed(file list). %d %ld\n", rv, rx_msg.size);
				rt_sem_release(sem_thread_exit);
				break;
			}
			
			if (s_flag == 0)
			{
#if 0
				s_flag = 1;
				memset(buff, 0, 4);
				rv = rt_device_read(serial, 0, buff, 4);
				if (rv <= 0)
				{
					printf("_thread_vcom_recv_dir read buff 4 failed(file list). %d %d\n", rv, s_len);
					break;
				}
#endif
				s_flag = 1;
				memcpy(&s_total_filecount, s_buf, 4);
				printf("file_count:%d \n", s_total_filecount);
				files_local = malloc(s_total_filecount * sizeof(char*));
				files_remote = malloc(s_total_filecount * sizeof(char*));
				RT_ASSERT(files_local != NULL);
				RT_ASSERT(files_remote != NULL);
				for (size_t i = 0; i < s_total_filecount; i++)
				{
					files_local[i] = malloc(MAX_PATH_LENGTH);
					files_remote[i] = malloc(MAX_PATH_LENGTH);
					RT_ASSERT(files_local[i] != NULL);
					RT_ASSERT(files_remote[i] != NULL);
					memset(files_local[i], 0, MAX_PATH_LENGTH);
					memset(files_remote[i], 0, MAX_PATH_LENGTH);
				}
			}
			else
			{
                #if 0
				memset(buff, 0, 4);
				memset(file_buf, 0, sizeof(file_buf));
				rv=rt_device_read(serial,0,buff,sizeof(buff));
				if (rv <= 0)
				{
					printf("recv failed(file list). %d %d\n", rv, s_len);
					break;
				}

				memcpy(&s_len_filename, buff, 4);
				printf("s_len_filename:%d\n",s_len_filename);
				printf("%02x %02x %02x %02x\n", buff[0], buff[1], buff[2], buff[3]);

				rv=rt_device_read(serial,0,file_buf,s_len_filename);
				if (rv <= 0)
				{
					printf("recv failed(file list). %d %d\n", rv, s_len);
					break;
				}
                #endif
				printf("file_path:%s,%s\n", s_buf,thread_param.local_path);
				strcpy(files_local[file_index], thread_param.local_path);
				strcat(files_local[file_index], s_buf);
				//strcpy(files_remote[file_index], thread_param.remote_path);
				strcat(files_remote[file_index], s_buf);
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
	}

	s_flag = 0;
	sum = 0;
	char dir_check[256];
	for (int i = 0; i < s_total_filecount; i++)
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

		while (1)
		{
            rt_memset(&rx_msg, 0, sizeof(rx_msg));
			result = rt_mq_recv(rx_mq_p, &rx_msg, sizeof(rx_msg), 300);
			rv = rt_device_read(serial, 0, s_buf, rx_msg.size);
            if (rv <= 0)
            {
                printf("\033[0m\033[41;33mrecv file %s failed(folder mode). %d\033[0m\n", files_local[i],rv);
                continue;
            }
			if (s_flag == 0)
			{
				s_flag = 1;
#if 0
                memset(buff, 0, 4);
                //rv = recv(link_id, buff, sizeof(buff), 0);
                rv=rt_device_read(serial,0,buff,sizeof(buff));
                if (rv <= 0)
                {
                    //printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
                    continue;
                }
#endif
				memcpy(&s_total_len, s_buf, 4);
				if (s_total_len == 0) break;
				file = (char*)malloc(s_total_len);
				printf("save to %s,total len: %d.\n", files_local[i],s_total_len);
                //printf("start: %x %x %x %x\n", buff[0], buff[1], buff[2], buff[3]);
			}
			else
			{
#if 0
				//rv = recv(link_id, file_buf, sizeof(file_buf), 0);
				rv=rt_device_read(serial,0,file_buf,sizeof(file_buf));
				if (rv <= 0)
				{
					printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
					continue;
				}
#endif
				memcpy(file+sum-4, s_buf, rv);
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
		for (size_t i = 0; i < s_total_filecount; i++)
		{
			free(files_local[i]);
			free(files_remote[i]);
		}
		free(files_local);
		free(files_remote);
	}

	s_total_filecount = 0;
	rt_sem_release(sem_thread_exit);
}

static void _thread_vcom_recv(void *parameter)
{
    int rv;
	char buf[MAX_PATH_LENGTH];
    int sum = 0;
	char *file=NULL;
	char** files_local = NULL;
	char** files_remote = NULL;
	struct rx_msg rx_msg;
	rt_err_t result;

	// read remote directory file list
	if (!thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		sprintf(buf, "read-dir-req:%s", thread_param.remote_path);
		rt_device_write(serial, 0, buf, MAX_PATH_LENGTH);
		rt_sem_take(tx_sem, RT_WAITING_FOREVER);
		int file_index = 0;

		while (1)
		{
			rt_memset(&rx_msg, 0, sizeof(rx_msg));
			result = rt_mq_recv(rx_mq_p, &rx_msg, sizeof(rx_msg), 300);
			rv = rt_device_read(serial, 0, s_buf, rx_msg.size);
			if (rv <= 0)
			{
				printf("recv failed(file list). %d %d\n", rv, s_len);
                rt_sem_release(sem_thread_exit);
				break;
			}

			if (s_flag == 0)
			{
				s_flag = 1;
				memcpy(&s_total_filecount, s_buf, 4);
				printf("file_count:%d \n", s_total_filecount);
				files_local = malloc(s_total_filecount * sizeof(char*));
				files_remote = malloc(s_total_filecount * sizeof(char*));
				RT_ASSERT(files_local != NULL);
				RT_ASSERT(files_remote != NULL);
				for (size_t i = 0; i < s_total_filecount; i++)
				{
					files_local[i] = malloc(MAX_PATH_LENGTH);
					files_remote[i] = malloc(MAX_PATH_LENGTH);
					RT_ASSERT(files_local[i] != NULL);
					RT_ASSERT(files_remote[i] != NULL);
					memset(files_local[i], 0, MAX_PATH_LENGTH);
					memset(files_remote[i], 0, MAX_PATH_LENGTH);
				}
			}
			else
			{
				//printf("file_path:%s \n", s_buf);
				strcpy(files_local[file_index], thread_param.local_path);
				strcat(files_local[file_index], s_buf);
				strcpy(files_remote[file_index], thread_param.remote_path);
				strcat(files_remote[file_index], s_buf);
				file_index++;
			}
			sum += rv;

			if (file_index >= s_total_filecount)
			{
				printf("total recv:%d \n", sum);
				break;
			}
		}
	}
	else if (thread_param.is_file_opt && thread_param.is_pull_opt)
	{
		s_total_filecount = 1;
		files_local = malloc(s_total_filecount * sizeof(char*));
		files_remote = malloc(s_total_filecount * sizeof(char*));
		files_local[0] = malloc(MAX_PATH_LENGTH);
		files_remote[0] = malloc(MAX_PATH_LENGTH);
		RT_ASSERT(files_local[0] != NULL);
		RT_ASSERT(files_remote[0] != NULL);
		memset(files_local[0], 0, MAX_PATH_LENGTH);
		memset(files_remote[0], 0, MAX_PATH_LENGTH);
		strcpy(files_remote[0], thread_param.remote_path);
		strcpy(files_local[0], thread_param.local_path);
	}

	s_flag = 0;
	sum = 0;
	char dir_check[256];
	for (int i = 0; i < s_total_filecount; i++)
	{
		memset(dir_check, 0, 256);
		strncpy(dir_check, files_local[i], strlen(files_local[i]) - strlen(strrchr(files_local[i], '/')));
		if (access(dir_check, F_OK) != 0)
		{
			create_dir1(dir_check);
		}
		s_fp = fopen(files_local[i], "wb+");
		if (s_fp == NULL) {
			printf("\033[0m\033[41;33mopen %s failed!\033[0m\n", files_local[i]);
			continue;
		}
        sprintf(buf, "read-req:%s", files_remote[i]);
        rt_device_write(serial, 0, buf, MAX_PATH_LENGTH);
		rt_sem_take(tx_sem, RT_WAITING_FOREVER);
		printf("\nrecv %s!\n", files_remote[i]);
		while (1)
		{
			rt_memset(&rx_msg, 0, sizeof(rx_msg));
			result = rt_mq_recv(rx_mq_p, &rx_msg, sizeof(rx_msg), 300);
			rv = rt_device_read(serial, 0, s_buf, rx_msg.size);
			if (rv <= 0)
			{
				printf("\033[0m\033[41;33mrecv failed(folder mode). %d\033[0m\n", rv);
                rt_sem_release(sem_thread_exit);
				break;
			}
			if (s_flag == 0)
			{
				printf("start: %x %x %x %x\n", s_buf[0], s_buf[1], s_buf[2], s_buf[3]);
				s_flag = 1;
				//s_total_len = *((int*)s_buf);
				memcpy(&s_total_len, s_buf, 4);
				if (s_total_len == 0) break;
				file = (char*)malloc(s_total_len);
				printf("total len: %d.\n", s_total_len);
				if (rv > 4)
				{
					memcpy(file, s_buf+4, rv-4);
					//fwrite(s_buf+4, rv-4, 1, s_fp);
				}
			}
			else
			{
				memcpy(file+sum-4, s_buf, rv);
				//printf("sum:%d ", sum);
				//fwrite(s_buf, rv, 1, s_fp);
			}

			sum += rv;
			if (sum >= s_total_len + 4)
			{
				fwrite(file, 1, s_total_len, s_fp);
				printf("download ok: %d.\n", sum);
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
		for (size_t i = 0; i < s_total_filecount; i++)
		{
			free(files_local[i]);
			free(files_remote[i]);
		}
		free(files_local);
		free(files_remote);
	}
	s_total_filecount = 0;

	rt_sem_release(sem_thread_exit);
}
static void _thread_vcom_send(void *parameter)
{
    int sum = 0;
	int filelen;
	char *file=NULL;
	char buf[MAX_PATH_LENGTH];

    //open
	s_fp = fopen(thread_param.local_path, "rb");
	if (s_fp == NULL) {
		printf("fopen %s failed!\n", thread_param.local_path);
		rt_sem_release(sem_thread_exit);
		return;
	}

	printf("fopen %s ok!\n", thread_param.local_path);
	memset(buf, 0, sizeof(buf));
	sprintf(buf, "send-req:%s", thread_param.remote_path);

	//cmd
	printf("send %s.!\n", buf);
	rt_device_write(serial, 0, buf, MAX_PATH_LENGTH);

    while(1)
    {
        rt_sem_take(tx_sem, RT_WAITING_FOREVER);
		if (s_flag == 0)
		{
			char bin[4];
			s_flag = 1;
			fseek(s_fp, 0, SEEK_END);
			filelen = ftell(s_fp);
			printf("+++++file len. %d.\n", filelen);
			file = (char*)malloc(filelen);
			fseek(s_fp, 0, SEEK_SET);
			fread(file, filelen, 1, s_fp);
			memcpy(bin, &filelen, 4);
			rt_device_write(serial, 0, bin, 4);
		}
		else
		{
			if (filelen - sum > PACKET_SIZE)
			{
				rt_device_write(serial, 0, file+sum, PACKET_SIZE);
				sum += PACKET_SIZE;
			}
			else
			{
				rt_device_write(serial, 0, file+sum, filelen - sum);
				sum = 0;
				s_flag = 0;
				free(file);
				file = NULL;
				rt_sem_take(tx_sem, RT_WAITING_FOREVER);
				break;
			}
		}
    }
	rt_sem_release(sem_thread_exit);
}

int acp(int argc ,char *argv[])
{
    if (argc!=4) {
        printf("param error!\n");
        return 0;
    }

    serial = rt_device_find("vcom");
    if (!serial) {
        printf("find vcom failed!\n");
        return -1;
    }
	rt_device_open(serial, RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX);
    rt_device_set_rx_indicate(serial, uart_input);
	rt_device_set_tx_complete(serial, uart_output);
	tx_sem = rt_sem_create("tx_sem", 0, RT_IPC_FLAG_FIFO);
	sem_file_send_ok = rt_sem_create("file_send_ok", 0, RT_IPC_FLAG_FIFO);
	rx_mq_p = rt_mq_create( "rx_mq", sizeof(struct rx_msg), 2048, RT_IPC_FLAG_FIFO );
	sem_thread_exit = rt_sem_create("exit", 0, RT_IPC_FLAG_FIFO);

	if (strstr(argv[2], "r:") != NULL) {	// 接收文件
		thread_param.is_file_opt = RT_TRUE;
		thread_param.is_pull_opt = RT_TRUE;
		strcpy(thread_param.remote_path, strrchr(argv[2], ':')+1);
		char *file_name = strrchr(argv[2], '/');
		if (file_name == NULL) {	// 当前目录下的文件, 可直接使用文件名
			snprintf(thread_param.local_path, MAX_PATH_LENGTH, "%s/%s",argv[3], strrchr(argv[2], ':')+1);
		} else {
			snprintf(thread_param.local_path, MAX_PATH_LENGTH, "%s/%s",argv[3], file_name+1);
		}
	} else if (strstr(argv[2], "rd:") != NULL) {	// 接收目录
		thread_param.is_file_opt = RT_FALSE;
		thread_param.is_pull_opt = RT_TRUE;
		strcpy(thread_param.local_path, argv[3]);
		snprintf(thread_param.remote_path, MAX_PATH_LENGTH, "%s", strrchr(argv[2], ':')+1);
	} else if (strstr(argv[3], "r:") != NULL){	// 发送文件
		thread_param.is_file_opt = RT_TRUE;
		thread_param.is_pull_opt = RT_FALSE;
		strcpy(thread_param.local_path, argv[2]);
         char *file_name = strrchr(argv[2], '/');
		if (file_name == NULL) {	// 当前目录下的文件, 可直接使用文件名
            snprintf(thread_param.remote_path, MAX_PATH_LENGTH, "%s/%s", strrchr(argv[3], ':')+1,argv[2]);
		} else {
            snprintf(thread_param.remote_path, MAX_PATH_LENGTH, "%s/%s", strrchr(argv[3], ':')+1,file_name+1);

		}
	}else if (strstr(argv[3], "rd:") != NULL){	// 发送文件目录
		thread_param.is_file_opt = RT_FALSE;
		thread_param.is_pull_opt = RT_FALSE;
		strcpy(thread_param.local_path, argv[2]);
        snprintf(thread_param.remote_path, MAX_PATH_LENGTH, "%s", strrchr(argv[3], ':')+1);
	}else{
	    printf("argument 2\3 is not correct!\n");
        goto exit;
    }

    // 如果传输的是目录，则加上本地路径结尾的"/"
	if (!thread_param.is_file_opt && thread_param.local_path[strlen(thread_param.local_path) - 1] != '/')
	{
		strcat(thread_param.local_path, "/");
	}

	// 如果传输的是目录，则加上远程路径结尾的"/"
	if (!thread_param.is_file_opt && thread_param.remote_path[strlen(thread_param.remote_path) - 1] != '/')
	{
		strcat(thread_param.remote_path, "/");
	}

    printf("is_file:%d, is_pull:%d\n", (int)thread_param.is_file_opt, (int)thread_param.is_pull_opt);
	printf("local path:%s\n", thread_param.local_path);
	printf("remote path:%s\n", thread_param.remote_path);

	if (strstr(argv[1], "rndis") != NULL){
		printf("using rndis to transmit file!\n");
		tool_flag = 0;
	}
	else if (strstr(argv[1], "vcom") != NULL){
		printf("using vcom to transmit file!\n");
		tool_flag = 1;
	}
	else {
		printf("argument 1 is not correct!\n");
		goto exit;
	}
	rt_thread_t thread;

	if (thread_param.is_file_opt && thread_param.is_pull_opt)	// 接收文件
	{
        if(0==tool_flag){
            thread = rt_thread_create("rndis_recv", _thread_recv, &thread_param, 10240, 25, 10);
        }
        else {
            thread = rt_thread_create("vcom_recv", _thread_vcom_recv, &thread_param, 10240, 25, 10);
        }

        if (thread != RT_NULL)
        {
            rt_thread_startup(thread);
        }
		rt_thread_delay(1);
		rt_sem_take(sem_thread_exit, RT_WAITING_FOREVER);
        printf("file recv exit thread\n");
	}
    else if(!thread_param.is_file_opt&& thread_param.is_pull_opt)//接收目录
    {
		if(0==tool_flag){
			thread = rt_thread_create("rndis_recv_dir", _thread_recv_dir, RT_NULL, 10240, 25, 10);
		}else{
			thread = rt_thread_create("vcom_recv_dir", _thread_vcom_recv_dir, RT_NULL, 10240, 25, 10);
		}
        if (thread != RT_NULL)
		{
			rt_thread_startup(thread);
		}
		rt_thread_delay(1);
		rt_sem_take(sem_thread_exit, RT_WAITING_FOREVER);
        printf("dir recv exit thread\n");
    }
	else if(thread_param.is_file_opt&& !thread_param.is_pull_opt)// 发送文件
	{
		if (0 == tool_flag){
			thread = rt_thread_create("rndis_send", _thread_send, RT_NULL, 10240, 25, 10);
		}
		else {
			thread = rt_thread_create("vcom_send", _thread_vcom_send, RT_NULL, 10240, 25, 10);
		}
		if (thread != RT_NULL)
		{
			rt_thread_startup(thread);
		}
		rt_thread_delay(1);
		rt_sem_take(sem_thread_exit, RT_WAITING_FOREVER);
		printf("send exit thread\n");
	}
    else if (!thread_param.is_file_opt&& !thread_param.is_pull_opt)// 发送文件目录
	{
         if( 0==tool_flag ){
        	thread = rt_thread_create("rndis_send_dir", _thread_send_dir, RT_NULL, 10240, 25, 10);
         }
         else{
            thread = rt_thread_create("vcom_send_dir", _thread_vcom_send_dir, RT_NULL, 10240, 25, 10);
         }
    		if (thread != RT_NULL)
    		{
    			rt_thread_startup(thread);
    		}

		rt_sem_take(sem_thread_exit, RT_WAITING_FOREVER);
        printf("send dir exit thread\n");
	}
	//rt_thread_delay(10);

exit:
	rt_sem_delete(sem_thread_exit);
	rt_sem_delete(tx_sem);
	rt_sem_delete(sem_file_send_ok);
	rt_mq_delete(rx_mq_p);
	serial = NULL;
	tx_sem = NULL;
	rx_mq_p = NULL;
	sem_thread_exit = NULL;
	sem_file_send_ok = NULL;
	if (sockfd){
		close(sockfd);
		printf("---------Transmiting is complete,close socket.\n");
		sockfd = -1;
	}
    return 0;
}

MSH_CMD_EXPORT(acp, e02 tool for transmiting files_local);
