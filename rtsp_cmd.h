#include <sys/types.h>

//typedef unsigned long u_int32_t;
typedef unsigned short u_int16_t;
typedef unsigned char u_int8_t;
typedef u_int16_t portNumBits;
typedef u_int32_t netAddressBits;
typedef long long _int64;

typedef enum
{
	RTP_UDP,
	RTP_TCP,
	RAW_UDP
}StreamingMode;

BOOL OptionAnswer(char *cseq, int sock);
BOOL DescribeAnswer(char *cseq,int sock,char* urlSuffix,char* recvbuf);
BOOL SetupAnswer(char *cseq,int sock,int SessionId ,char * urlSuffix,char* recvbuf,int* rtpport, int* rtcpport);
BOOL PlayAnswer(char *cseq,int sock,int SessionId,char* urlPre,char * recvbuf);
BOOL PauseAnswer(char *cseq,int sock,char * recvbuf);
BOOL TeardownAnswer(char *cseq,int sock,int SessionId,char * recvbuf);

BOOL ParseRequestString(char const* reqStr,
		unsigned reqStrSize,
		char* resultCmdName,
		unsigned resultCmdNameMaxSize,
		char* resultURLPreSuffix,
		unsigned resultURLPreSuffixMaxSize,
		char* resultURLSuffix,
		unsigned resultURLSuffixMaxSize,
		char* resultCSeq,
		unsigned resultCSeqMaxSize);

void ParseTransportHeader(char const* buf,
		StreamingMode * streamingMode,			 
		char** streamingModeString,			  
		char** destinationAddressStr,			  
		u_int8_t * destinationTTL,			  
		portNumBits* clientRTPPortNum, // if UDP			  
		portNumBits* clientRTCPPortNum, // if UDP			  
		unsigned char* rtpChannelId, // if TCP		  
		unsigned char* rtcpChannelId // if TCP
		);



