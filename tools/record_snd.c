/*
 * Copyrighted as an unpublished work.
 * (c) Copyright 1991 Brian Smith
 * All rights reserved.
 *
 * Read the LICENSE file for details on distribution and use.
 *
 * March 1992 - Lance Norskog
 * hack to set priority high if super-user : ROOTHACK
 * 
 */

#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/lock.h>			/* ROOTHACK */
#include <sys/sb.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>

#define NUMBUFS     4
#define SHM_BUFSIZ  (8*4096)
#define SHM_KEY     1796

typedef struct {
    char    buf[NUMBUFS*SHM_BUFSIZ];
    int     write_waiting;
    int     read_waiting;
    int     locked[NUMBUFS];
    int     length[NUMBUFS];
}   buf_struct;


/* GLOBALS */
int shmid;
buf_struct *buffers;

void sigusr_handler()
{
    return;
}


void cleanup()
{
    printf("cleanup\n");
    shmctl(shmid, IPC_RMID, 0);
    exit(0);
}

void detach()
{
    printf("detaching\n");
    shmdt(buffers);
    exit(0);
}


int main(argc, argv)
int argc;
char **argv;
{
    int i;
    int outfile;
    int sound_fd;
    int child_pid;
    int buf_num;
    int rc;

    if (argc != 2)
    {
        printf("usage: %s <new sound file>\n", argv[0]);
        printf("\tif <sound file> is \"-\", then %s will record to stdout\n",
            argv[0]);
        exit(-1);
    }

    nice(-15);				/* ROOTHACK */
    plock(PROCLOCK);			/* ROOTHACK */
    setuid(getuid());			/* ROOTHACK */

    if (strcmp(argv[1], "-") == 0)
        outfile = 1;
    else
    {
        outfile = open(argv[1], O_WRONLY | O_CREAT, 0666);
        if (outfile == -1)
        {
            perror("opening output file");
            printf("usage: %s <output sound file>\n", argv[0]);
            printf("\tif <sound file> is \"-\", %s will record to stdout\n",
                argv[0]);
            exit(-1);
        }
    }

    /* open device */
    sound_fd = open("/dev/sbdsp", O_RDONLY);
    if (sound_fd == -1)
    {
        perror("opening SoundBlaster device");
        exit(-1);
    }
    if (ioctl(sound_fd, DSP_IOCTL_RESET) == -1)
    {
        perror("trying to reset DSP");
        exit(-1);
    }
    if (ioctl(sound_fd, DSP_IOCTL_VOICE, 0) == -1)
    {
        perror("trying to set voice on");
        exit(-1);
    }

    /* create shared memory segment */
    shmid = shmget(SHM_KEY, sizeof(buf_struct), IPC_CREAT | IPC_EXCL | 0644);
    if (shmid == -1)
    {
        perror("creating shared-mem buffer");
        exit(-1);
    }

    /* attach handler for signal */
    if (sigset(SIGUSR1, sigusr_handler) == SIG_IGN)
	    sigset(SIGUSR1, SIG_IGN);
    for(i = 1; i < SIGUSR1; i++)
	    if (sigset(i, cleanup) == SIG_IGN)
		    sigset(i, SIG_IGN);

    /* start read process */
    child_pid = fork();
    switch (child_pid)
    {
        case 0:
            start_read(sound_fd);
            exit(0);
        case -1:
            perror("forking read process");
            cleanup();
    }

    /* attach shared memory segment */
    buffers = (buf_struct *)shmat(shmid, 0, 0);
    if (buffers == (buf_struct *)-1)
    {
        perror("attaching shared memory");
        if (buffers->read_waiting)
            kill(child_pid, SIGKILL);
        cleanup();
    }

    /* start writing stuff from buffers */
    while(1)
    {
        /* wait until buffer is locked for us, or flush and break on eof */
        if (!buffers->locked[buf_num])
        {
            buffers->write_waiting = 1;
            sigpause(SIGUSR1);
            continue;
        }

        /* not waiting now */
        buffers->write_waiting = 0;

        /* eof check */
        if (buffers->length[buf_num] <= 0)
            break;

        /* write out data in buffer */
        rc = write(outfile, buffers->buf + (buf_num*SHM_BUFSIZ),
            buffers->length[buf_num]);
        if (rc != buffers->length[buf_num])
        {
            if ((errno == EINTR) || (errno == 0))
                continue;

            perror("writing to output file");
            kill(child_pid, SIGKILL);
            cleanup();
        }

        /* unlock buffer for child's use */
        buffers->locked[buf_num] = 0;
        if (buffers->read_waiting)
            kill(child_pid, SIGUSR1);

        /* go to next buffer */
        buf_num++;
        buf_num %= NUMBUFS;
    }


    printf("done\n");
    cleanup();
    return(0);
}

int start_read(sound_fd)
int sound_fd;
{
    buf_struct *buffers;
    int buf_num = 0;

    /* attach handler for signal */
    sigset(SIGUSR1, sigusr_handler);
    sigset(SIGINT, detach);
    sigset(SIGHUP, detach);

    /* attach shared memory */
    buffers = (buf_struct *)shmat(shmid, 0, 0);
    if (buffers == (buf_struct *)-1)
    {
        perror("attaching shared memory");
        exit(0);
    }

    for (buf_num=0; buf_num < NUMBUFS; buf_num++)
        buffers->locked[buf_num] = 0;

    /* start reading into buffers */
    buf_num = 0;
    sleep(1);
    while(1)
    {
        /* wait for current buffer to become unlocked */
        if (buffers->locked[buf_num])
        {
            buffers->read_waiting = 1;
            sigpause(SIGUSR1);
            continue;
        }

        /* not waiting any more */
        buffers->read_waiting = 1;

        /* actually read data */
        buffers->length[buf_num] =
            read(sound_fd, buffers->buf + (buf_num*SHM_BUFSIZ), SHM_BUFSIZ);
        if (buffers->length[buf_num] == -1) 
        {
            if (errno == EINTR)
                continue;
            else
            {
                perror("reading from SoundBlaster\n");
                detach();
            }
        }
        else if (buffers->length[buf_num] == 0)
        {
            buffers->locked[buf_num] = 1;

            /* wake up parent */
            if (buffers->write_waiting)
                kill(getppid(), SIGUSR1);

            break;
        }

        /* lock buffer for parent's use */
        buffers->locked[buf_num] = 1;
                
        /* wake up parent */
        if (buffers->write_waiting)
            kill(getppid(), SIGUSR1);

        /* go to next buffer */
        buf_num++;
        buf_num %= NUMBUFS;
    }
        

    /* wake up parent */
    kill(getppid(), SIGUSR1);

    /* detach shared memory */
    shmdt(buffers);

    return(0);
}
