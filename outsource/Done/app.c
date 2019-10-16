#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#define IOCTL_APP_TYPE 71
#define READ_VALUE _IOR(IOCTL_APP_TYPE,2,int32_t*)

int32_t value;

void read_vars(int fd)
{
    int ret;
    if(ioctl(fd, READ_VALUE, (int32_t*) &value) == -1)
        printf("READ VALUE ERROR %d",ret);
    else printf("Value is %d\n", value);
}

int main(int argc, char *argv[])
{
    char *file_name = "/dev/srf05";
    int fd;
    
    fd = open(file_name, O_RDONLY);
    if(fd < 0) 
    {
        printf("Cannot open device file...\n");
        return 0;
    } else read_vars(fd);

    close(fd);
    return 0;
}