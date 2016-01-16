#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

int sock=-1;
#define MAXCLIENT 10
int clientfd[MAXCLIENT] = {-1};

#define PORT 31975

int sock_open(int port)
{
	struct sockaddr_in name;
	sock = socket (PF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		perror ("socket");
		sock = -1;
		return -1;
	}
	int reuse = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
        	perror("setsockopt(SO_REUSEADDR) failed");

#ifdef SO_REUSEPORT
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse)) < 0) 
        	perror("setsockopt(SO_REUSEPORT) failed");
#endif
	name.sin_family = AF_INET;
	name.sin_port = htons (port);
	name.sin_addr.s_addr = htonl (INADDR_ANY);
	if (bind (sock, (struct sockaddr *) &name, sizeof (name)) < 0) {
		perror ("bind");
		close(sock);
		sock = -1;
		return -1;
	}
	if (listen (sock, 5) < 0) {
		perror ("listen");
		close(sock);
		sock = -1;
		return -1;
	}
	return 0;
}

int accept_client() {
	int new;
	struct sockaddr_in saddr;
	int saddr_len = sizeof(saddr);
	new = accept(sock, (struct sockaddr *)&saddr, &saddr_len);
        if (new < 0) {
		perror ("accept");
		return -1;
	}
	fprintf (stderr, "reflector-mute: connect from host %s, port %d.\n",
                         inet_ntoa (saddr.sin_addr),
                         ntohs (saddr.sin_port));
	int i;
	for(i=0; i<MAXCLIENT; i++) {
		if(clientfd[i]==-1) {
			clientfd[i] = new;
			return new;
		}
	}
	fprintf(stderr, "no free client slot\n");
	close(new);
}

int distribute_clients(char c) {
	int i;
	char buf[1];
	buf[0] = c;
	for(i=0; i<MAXCLIENT; i++) {
		if(clientfd[i]<=0) continue;
		int res = write(clientfd[i], &buf, 1);
		if(res<0) {
			fprintf(stderr, "Connected to fd %d failed\n", clientfd[i]);
			close(clientfd[i]);
			clientfd[i] = -1;
		}
	}
}

int ptym_open(char *pts_name, int pts_namesz)
{
	char	*ptr;
	int		fdm;

	/*
	 * Return the name of the master device so that on failure
	 * the caller can print an error message.  Null terminate
	 * to handle case where strlen("/dev/ptmx") > pts_namesz.
	 */
	strncpy(pts_name, "/dev/ptmx", pts_namesz);
	pts_name[pts_namesz - 1] = '\0';
	if ((fdm = open(pts_name, O_RDWR| O_NOCTTY)) < 0)
		return(-1);
	if (grantpt(fdm) < 0) {		/* grant access to client */
		close(fdm);
		return(-2);
	}
	if (unlockpt(fdm) < 0) {	/* clear client's lock flag */
		close(fdm);
		return(-3);
	}
	if ((ptr = ptsname(fdm)) == NULL) {	/* get client's name */
		close(fdm);
		return(-4);
	}

#if 1
	struct termios tp;
    	if (tcgetattr(fdm, &tp) == -1) {
		perror("tcgetattr"); return -5;
	}
	tp.c_iflag = 0;
	tp.c_oflag = 0;
	tp.c_cflag &= ~CSIZE;
	tp.c_cflag &= ~PARENB;
    	tp.c_cflag |= CS8;
    	tp.c_lflag &= ~ECHO;                /* ECHO off, other bits unchanged */
    	tp.c_lflag &= ~ICANON;
    	tp.c_lflag &= ~ISIG;
    	if (tcsetattr(fdm, TCSAFLUSH, &tp) == -1) {
		perror("tcsetattr"); return -6;
	}
#endif
	/*
	 * Return name of client.  Null terminate to handle
	 * case where strlen(ptr) > pts_namesz.
	 */
	strncpy(pts_name, ptr, pts_namesz);
	pts_name[pts_namesz - 1] = '\0';
	return(fdm);			/* return fd of master */
}

#define MAXNAME 200
int undoRename(char *dname)
{
	char oname[MAXNAME];
	snprintf(oname, MAXNAME, "%s-orig", dname);

	struct stat sresult;
	int res = stat(oname, &sresult);
	if(res<0) {
		fprintf(stderr,"Renamed name does not exist, skip the undoRename step\n");
		return 0;
	}
	fprintf(stderr,"Removing fake %s, restoring original from %s\n",dname, oname);
	res = unlink(dname);
	if(res<0) { perror(dname); }

	res = rename(oname, dname);
	if(res<0) {
		perror("rename failed");
		return -1;
	}
	return 0;
}
int renameDevice(char *dname)
{
	char oname[MAXNAME];
	snprintf(oname, MAXNAME, "%s-orig", dname);

	struct stat sresult;
	int res = stat(oname, &sresult);
	if(res<0) {
		fprintf(stderr,"Old name: %s, new name: %s\n",dname, oname);
		res = rename(dname, oname);
		if(res<0) {
			perror("rename failed");
			return -1;
		}
	} else if(res==0) {
		/* device-orig alread exists, skip the rename step */
		fprintf(stderr,"Renamed device already exists, not renaming...\n");
		// Remove old thing if it is a link
		int res = lstat(dname, &sresult);
		if(S_ISLNK(sresult.st_mode))
			unlink(dname);
	}
	res = open(oname, O_RDWR);
	if(res<0) {
		perror(oname);
		undoRename(dname);
		return -1;
	}
	return res;
}

int running;
#define MAXBUF 4096


enum { SEARCHING, MATCHING, READLEN, CLEARING };
int vfstate = SEARCHING;
int vfpos = 0;
char pattern[]="\x71\xfe\x39\x1d\x07";
int enabled = 0;
char crc;

#if 0
int logenabled=0;
int logfd=-1;
void openlog() {
	if(logfd>0) close(logfd);
	logfd = open("/tmp/fusion-log", O_WRONLY|O_CREAT|O_APPEND, S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);
}

void closelog() {
	if(logfd>0) {
		close(logfd);
		logfd=-1;
	}
}
#endif

void voicefilter(char *buf, int len) {
	char *end = buf+len;
	while(len--) {
		char c = *buf; buf++;
		if(vfstate != CLEARING) { crc = crc^c; }

		switch(vfstate) {
		case MATCHING:
			if(pattern[vfpos]==c) {
				vfpos++;
				if(vfpos+1>=sizeof(pattern)) vfstate = READLEN;
				continue;
			}
			vfstate = SEARCHING;
			// intentional fall-through
		case SEARCHING:
			if(pattern[0]==c) { crc=c; vfpos=1; vfstate=MATCHING; continue; }
			continue;
		case READLEN:
			vfpos = (int)(unsigned char)c;
			if(vfpos>0) vfstate = CLEARING;
			else vfstate = SEARCHING;
			continue;
		case CLEARING:
			distribute_clients(c);
#if 0
			if(logenabled==2) {
				openlog();
				logenabled=1;
			} else if (logfd>0 && logenabled==0) {
				closelog();
			}
			if(logfd>0 && logenabled) {
				write(logfd, buf-1, 1);
			}
#endif
			if(enabled) {
				*(buf-1) = 0;
			}
			if(--vfpos > 0) continue;
			vfstate = SEARCHING;
			if(enabled && buf<end) *buf=crc;
		}
	}
}


int forward(int fromfd, int tofd, void (*filter)(char *, int))
{
	char buf[MAXBUF];
	int res = read(fromfd, buf, MAXBUF);
	if(res<0) {
		if(errno!=EINTR) running = 0;
		perror("read: pseudo_to_serial");
		return 0;
	}
	if(res==0) {
		return 0;
	}
	if(filter) filter(buf, res);

	int res2 = write(tofd, buf, res);
	if(res2<0) {
		if(errno!=EINTR) running = 0;
		perror("write: pseudo_to_serial");
		return 0;
	}
	return 0;
}

int serial_to_pseudo(int fromfd, int tofd)
{
	return forward(fromfd, tofd, voicefilter);
}

int pseudo_to_serial(int fromfd, int tofd)
{
	return forward(fromfd, tofd, NULL);
}

int copyLoop(int serfd, int ptyfd)
{
	fd_set readset;
	struct timeval to;
	int maxfd = 1 + (serfd>ptyfd?serfd:ptyfd);
	int res = sock_open(PORT);
	if(res<0) return;
	if(maxfd<=sock) maxfd=sock+1;
	while(running) { 
		FD_ZERO(&readset);
		FD_SET(serfd, &readset);
		FD_SET(ptyfd, &readset);
		FD_SET(sock, &readset);
	//for(int i=0; i<MAXCLIENT; i++) {
	//		if(clientfd[i]>0) FD_SET(clientfd[i], &readset);
	//	}
		to.tv_sec=1;
		to.tv_usec=0;
		int act = select(maxfd, &readset, NULL, NULL, &to);
		if(act<0 && errno!=EINTR) {
			perror("select");
			return -1;
		}
		if(act<0 && errno==EINTR) {
			continue;
		}
		if(FD_ISSET(serfd, &readset)) {
			serial_to_pseudo(serfd, ptyfd);
		}
		if(FD_ISSET(ptyfd, &readset)) {
			pseudo_to_serial(ptyfd, serfd);
		}
		if(FD_ISSET(sock, &readset)) {
			accept_client();
		}
	}
	return 0;
}

void sigHandler(int dummy) {
	running = 0;
}


// 1st time; enable/disable muting, 2nd time: enable/disable logging
void sigOnOff(int sig) {
	if(sig==SIGUSR1) {
		//if(enabled) logenabled = 2;
		enabled = 1;
	}
	else {
		//if(!enabled) logenabled = 0;
		enabled = 0;
	}
}

int main(int argc, char *argv[])
{
	int serfd, ptyfd;
	char name[MAXNAME];	
	char *devname;

	if(argc<2) {
		printf("Usage: %s <devicename>\n", argv[0]);
		exit(1);
	}
	devname = argv[1];

	/* rename device and open it */
	serfd = renameDevice(devname);
	if(serfd<0) {
		fprintf(stderr,"failed.\n");
		exit(1);
	}
	
	/* open pseudo tty */
	ptyfd = ptym_open(name, MAXNAME);
	if(ptyfd<0) {
		fprintf(stderr,"Error: cannot open %s\n",name);	
		undoRename(devname);
		exit(1);
	}

	/* Link original device name to other end of pseudo tty */
	int res = symlink(name, devname);
	if(res<0) {
		perror("Creating link failed\n");
		undoRename(devname);
		exit(1);
	}

	int tmpfd = open(devname, O_RDWR|O_NOCTTY);
	fprintf(stderr,"Rpt telemetry C4FM reflector mute is running\n");
	running = 1;

	struct sigaction new_act, old_act;
	sigemptyset(&new_act.sa_mask);
	new_act.sa_flags = 0;
	new_act.sa_handler = sigHandler;

	signal(SIGPIPE, SIG_IGN);

	sigaction(SIGINT, &new_act, &old_act);
	sigaction(SIGQUIT, &new_act, &old_act);

	new_act.sa_handler = sigOnOff;
	new_act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &new_act, &old_act);
	sigaction(SIGUSR2, &new_act, &old_act);
	copyLoop(serfd, ptyfd);

	fprintf(stderr,"Rpt telemetry C4FM reflector mute is leaving\n");
	undoRename(devname);
	exit(0);
}

