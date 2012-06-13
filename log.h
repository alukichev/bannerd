#ifndef _LOG_H
#define _LOG_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#define LOG(pri, msg, ...) do { \
		if(Interactive) { \
			fprintf(stderr, "%d [%s:%d]: " msg "\n", (pri), __FILE__, __LINE__, ##__VA_ARGS__); \
			fflush(stderr);								\
		}										\
		else \
			syslog(pri, "[%s:%d]: " msg, __FILE__, __LINE__, ##__VA_ARGS__); \
	} while(0)

#define ERR(msg, ...) do {						     \
		LOG(LOG_ERR, msg, ##__VA_ARGS__);						\
		LOG(LOG_ERR,  "\nErrno: %d (%s)\n", errno, strerror(errno));			\
	} while(0)

#define ERR_RET(code, msg...) do {					 \
		ERR(msg);									 \
		return code;								 \
	} while(0)



extern int Interactive;

#endif /* _LOG_H */
