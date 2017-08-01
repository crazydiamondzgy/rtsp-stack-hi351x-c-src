#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include "hi_head.h"
#include "hi_init.h"
#include "hi_out.h"
#include "hi_log.h"
#include "hi_pthread_wait.h"
#include "hi_tran_dict.h"
#include "hi_adpt_socket.h"
#include "hi_transfer.h"
#include "hi_dbg.h"
#include <sys/ioctl.h>
#include "damd_socket.h"

#include "rtsp_cmd.h"
#include "rtsp_server.h"

#define MAX_RTSP_CHAN  2
#define RTP_H264               96
#define RTP_G726               97
#define MAX_RTSP_CLIENT       10
#define MAX_RTP_PKT_LENGTH   1400
#define RTSP_SERVER_PORT      554
#define RTSP_RECV_SIZE        1024
#define RTSP_MAX_VID          (1*1024*1024)
#define RTSP_MAX_AUD          (256*1024)
#define DEST_IP                "192.168.45.250"
#define DEST_PORT              5000

#define PARAM_STRING_MAX        100


RTP_FIXED_HEADER  *rtp_hdr;
NALU_HEADER		  *nalu_hdr;
FU_INDICATOR	  *fu_ind;
FU_HEADER		  *fu_hdr;

typedef enum {
	RTSP_IDLE = 0,
	RTSP_CONNECTED = 1,
	RTSP_SENDING = 2,
} RTSP_STATUS;

typedef struct {
	int  nVidLen;
	int  nAudLen;
	BOOL bIsIFrm;
	BOOL bWaitIFrm;
	BOOL bIsFree;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
} RTSP_PACK;

typedef struct {
	int index;
	int socket;
	int reqchn;
	int seqnum;
	unsigned int tsvid;
	unsigned int tsaud;
	int status;
	int sessionid;
	int rtpport;
	int rtcpport;
	char IP[20];
	char urlPre[PARAM_STRING_MAX];
} RTSP_CLIENT;

typedef struct {
	int  vidLen;
	int  audLen;
	int  nFrameID;
	char vidBuf[RTSP_MAX_VID];
	char audBuf[RTSP_MAX_AUD];
} FRAME_PACK;

FRAME_PACK g_FrmPack[MAX_RTSP_CHAN];
RTSP_PACK g_rtpPack[MAX_RTSP_CHAN];
RTSP_CLIENT g_rtspClients[MAX_RTSP_CLIENT];

int g_nSendDataChn = -1;
pthread_mutex_t g_mutex;
pthread_cond_t  g_cond;
pthread_mutex_t g_sendmutex;

pthread_t g_SendDataThreadId = 0;

void * RtspClientMsg(void*pParam) {
	pthread_detach(pthread_self());
	int nRes;
	char pRecvBuf[RTSP_RECV_SIZE];
	RTSP_CLIENT * pClient = (RTSP_CLIENT*)pParam;
	memset(pRecvBuf,0,sizeof(pRecvBuf));
	printf("RTSP:-----Create Client %s\n",pClient->IP);
	while(pClient->status != RTSP_IDLE) {
		nRes = recv(pClient->socket, pRecvBuf, RTSP_RECV_SIZE,0);
		if(nRes < 1) {
			printf("RTSP:Recv Error--- %d\n",nRes);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			HI_CloseSocket(pClient->socket);
			break;
		}
		char cmdName[PARAM_STRING_MAX];
		char urlPreSuffix[PARAM_STRING_MAX];
		char urlSuffix[PARAM_STRING_MAX];
		char cseq[PARAM_STRING_MAX];
		
		ParseRequestString(pRecvBuf,nRes,cmdName,sizeof(cmdName),urlPreSuffix,sizeof(urlPreSuffix),urlSuffix,sizeof(urlSuffix),cseq,sizeof(cseq));
		
		char *p = pRecvBuf;
		
		printf("<<<<<%s\n",p);
		
		if(strstr(cmdName, "OPTIONS")) {
			OptionAnswer(cseq,pClient->socket);
		} else if(strstr(cmdName, "DESCRIBE")) {
			DescribeAnswer(cseq,pClient->socket,urlSuffix,p);
		} else if(strstr(cmdName, "SETUP")) {
			int rtpport,rtcpport;
			SetupAnswer(cseq,pClient->socket,pClient->sessionid,urlPreSuffix,p,&rtpport,&rtcpport);
			g_rtspClients[pClient->index].rtpport = rtpport;
			g_rtspClients[pClient->index].rtcpport= rtcpport;
			g_rtspClients[pClient->index].reqchn = atoi(urlPreSuffix);
			if(strlen(urlPreSuffix)<100) {
				strcpy(g_rtspClients[pClient->index].urlPre,urlPreSuffix);
			}
		} else if(strstr(cmdName, "PLAY")) {
			PlayAnswer(cseq,pClient->socket,pClient->sessionid,g_rtspClients[pClient->index].urlPre,p);
			g_rtspClients[pClient->index].status = RTSP_SENDING;
			usleep(100);
		} else if(strstr(cmdName, "PAUSE")) {
			PauseAnswer(cseq,pClient->socket,p);
		} else if(strstr(cmdName, "TEARDOWN")) {
			TeardownAnswer(cseq,pClient->socket,pClient->sessionid,p);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			HI_CloseSocket(pClient->socket);
		}
	}
	printf("RTSP:-----Exit Client %s\n",pClient->IP);
	return NULL;
}

void * RtspServerListen(void*pParam) {
	HI_S32 s32Socket;
	struct sockaddr_in servaddr;
	HI_S32 s32CSocket;
    HI_S32 s32Rtn;
    HI_S32 s32Socket_opt_value = 1;
	int nAddrLen;
	SOCKADDR_IN addrAccept;
	BOOL bResult;
	
	memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(RTSP_SERVER_PORT); 
	
	s32Socket = HI_Socket(AF_INET, SOCK_STREAM, 0);
	
	if (HI_SetSockOpt(s32Socket ,SOL_SOCKET,SO_REUSEADDR,&s32Socket_opt_value,sizeof(int)) == -1) {
        WRITE_LOG_NORMAL("HI_SetSockOpt Error!");
        return (HI_VOID *)HI_ERR_SERVER_SETSOCKOPT;
    }

    s32Rtn = HI_Bind(s32Socket, (struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
    if (s32Rtn < 0) {
        HI_OUT_Printf(0, "RTSP listening on port %d, bind err, %d \n", RTSP_SERVER_PORT, s32Rtn);
        return (HI_VOID *)HI_ERR_SERVER_BIND;
    }
    
    s32Rtn = HI_Listen(s32Socket, 50);
    if (s32Rtn < 0) {
        HI_OUT_Printf(0, "RTSP listening on port %d, listen err, %d \n", RTSP_SERVER_PORT, s32Rtn);
        return (HI_VOID *)HI_ERR_SERVER_LISTEN;
    }
	
    HI_OUT_Printf(0, "\n=========================\n");
    HI_OUT_Printf(0, "RTSP listening on port %d\n", RTSP_SERVER_PORT);
    HI_OUT_Printf(0, "\n=========================\n");
	
	nAddrLen = sizeof(SOCKADDR_IN);
	int nSessionId = 1000;
    while ((s32CSocket = HI_Accept(s32Socket, (struct sockaddr*)&addrAccept, &nAddrLen)) >= 0) {
		printf("<<<<RTSP Client %s Connected...\n", inet_ntoa(addrAccept.sin_addr));
	
		int nMaxBuf = 10 * 1024;
		if(setsockopt(s32CSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nMaxBuf, sizeof(nMaxBuf)) == -1)
			printf("RTSP:!!!!!! Enalarge socket sending buffer error !!!!!!\n");
		int i;
		BOOL bAdd=FALSE;
		for(i=0;i<MAX_RTSP_CLIENT;i++) {
			if(g_rtspClients[i].status == RTSP_IDLE) {
				memset(&g_rtspClients[i],0,sizeof(RTSP_CLIENT));
				g_rtspClients[i].index = i;
				g_rtspClients[i].socket = s32CSocket;
				g_rtspClients[i].status = RTSP_CONNECTED;
				g_rtspClients[i].sessionid = nSessionId++;
				strcpy(g_rtspClients[i].IP,inet_ntoa(addrAccept.sin_addr));
				pthread_t threadId = 0;
				pthread_create(&threadId, NULL, RtspClientMsg, &g_rtspClients[i]);
				bAdd = TRUE;
				break;
			}
		}

		if(bAdd==FALSE) {
			memset(&g_rtspClients[0],0,sizeof(RTSP_CLIENT));
			g_rtspClients[0].index = 0;
			g_rtspClients[0].socket = s32CSocket;
			g_rtspClients[0].status = RTSP_CONNECTED;
			g_rtspClients[0].sessionid = nSessionId++;
			strcpy(g_rtspClients[0].IP,inet_ntoa(addrAccept.sin_addr));
			pthread_t threadId = 0;
			pthread_create(&threadId, NULL, RtspClientMsg, &g_rtspClients[0]);
			bAdd = TRUE;
		}		
    }

    if(s32CSocket < 0) {
        HI_OUT_Printf(0, "RTSP listening on port %d,accept err, %d\n", RTSP_SERVER_PORT, s32CSocket);
    }
	
	printf("----- INIT_RTSP_Listen() Exit !! \n");
	
	return NULL;
}

void* SendDataThread(void*pParam) {
	pthread_detach(pthread_self());
	printf("RTSP:-----create send thread\n");
	int udpfd;
	udpfd = socket(AF_INET,SOCK_DGRAM,0);
	while(TRUE) {
		pthread_mutex_lock(&g_mutex);
		pthread_cond_wait(&g_cond, &g_mutex);
		pthread_mutex_unlock(&g_mutex);
		
		int i=0;
		int nChanNum=0;
		for(i=0;i<MAX_RTSP_CLIENT;i++) {
			if(g_rtspClients[i].status!=RTSP_SENDING) {
				continue;
			}

			int heart = g_rtspClients[i].seqnum % 100;
			if(heart==0 && g_rtspClients[i].seqnum!=0) {
				char buf[1024];
				memset(buf,0,1024);
				char *pTemp = buf;
				pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\nPublic: %s\r\n\r\n",
					0,"OPTIONS,DESCRIBE,SETUP,PLAY,PAUSE,TEARDOWN");
				
				int reg = send(g_rtspClients[i].socket, buf,strlen(buf),0);
				if(reg <= 0) {
					printf("RTSP:Send Error---- %d\n",reg);
					g_rtspClients[i].status = RTSP_IDLE;
					g_rtspClients[i].seqnum = 0;
					g_rtspClients[i].tsvid = 0;
					g_rtspClients[i].tsaud = 0;
					HI_CloseSocket(g_rtspClients[i].socket);
					continue;
				} else {
				}
			}

			nChanNum = g_rtspClients[i].reqchn;
			if(nChanNum<0 || nChanNum>=MAX_RTSP_CHAN || nChanNum != g_nSendDataChn) {
				continue;
			}
			struct sockaddr_in server;
			int len =sizeof(server);
			int retval;
			server.sin_family=AF_INET;
			server.sin_port=htons(g_rtspClients[i].rtpport);          
			server.sin_addr.s_addr=inet_addr(g_rtspClients[i].IP);
			int	bytes=0;
			float framerate=25;
			unsigned int timestamp_increse=0;
			timestamp_increse=(unsigned int)(90000.0 / framerate);
			
			char* nalu_payload;
			int nAvFrmLen = 0;
			int nIsIFrm = 0;
			int nNaluType = 0;
			char sendbuf[140*1024];
			int nReg;
			
			nIsIFrm = g_rtpPack[nChanNum].bIsIFrm;
			nAvFrmLen = g_rtpPack[nChanNum].nVidLen;
			
			rtp_hdr =(RTP_FIXED_HEADER*)&sendbuf[0]; 
			rtp_hdr->payload   = RTP_H264;
			rtp_hdr->version   = 2;
			rtp_hdr->marker    = 0;
			rtp_hdr->ssrc      = htonl(10);
			
			if(nAvFrmLen<=1400) {
				rtp_hdr->marker = 1;
				rtp_hdr->seq_no = htons(g_rtspClients[i].seqnum++);
				nalu_hdr        = (NALU_HEADER*)&sendbuf[12]; 
				nalu_hdr->F     = 0; 
				nalu_hdr->NRI   = nIsIFrm; 
				nalu_hdr->TYPE  = nNaluType;
				
				nalu_payload=&sendbuf[13];
				memcpy(nalu_payload,g_rtpPack[nChanNum].vidBuf,g_rtpPack[nChanNum].nVidLen);
				g_rtspClients[i].tsvid=g_rtspClients[i].tsvid+timestamp_increse;
				
				rtp_hdr->timestamp=htonl(g_rtspClients[i].tsvid);
				bytes=g_rtpPack[nChanNum].nVidLen + 13 ;				
				sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
			} else if(nAvFrmLen>1400) {
				int k=0,l=0;
				k=nAvFrmLen/1400;
				l=nAvFrmLen%1400;
				int t=0;
				g_rtspClients[i].tsvid=g_rtspClients[i].tsvid+timestamp_increse;
				rtp_hdr->timestamp=htonl(g_rtspClients[i].tsvid);
				while(t<=k) {
					rtp_hdr->seq_no = htons(g_rtspClients[i].seqnum++);
					if (t == 0) {
						rtp_hdr->marker=0;
						fu_ind =(FU_INDICATOR*)&sendbuf[12];
						fu_ind->F= 0;
						fu_ind->NRI= nIsIFrm;
						fu_ind->TYPE=28;
						
						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->E=0;
						fu_hdr->R=0;
						fu_hdr->S=1;
						fu_hdr->TYPE=nNaluType;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].vidBuf,1400);//È¥µôNALUÍ·
						
						bytes=1400+14;						
						sendto( udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
						
					} else if(k==t) {
						rtp_hdr->marker=1;
						fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
						fu_ind->F= 0;
						fu_ind->NRI= nIsIFrm;
						fu_ind->TYPE=28;
						
						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->R=0;
						fu_hdr->S=0;
						fu_hdr->TYPE= nNaluType;
						fu_hdr->E=1;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].vidBuf+t*1400,l);
						bytes=l+14;		
						sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
					} else if(t<k && t!=0) {
						rtp_hdr->marker=0;
						fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
						fu_ind->F=0;
						fu_ind->NRI=nIsIFrm;
						fu_ind->TYPE=28;
						
						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->R=0;
						fu_hdr->S=0;
						fu_hdr->E=0;
						fu_hdr->TYPE=nNaluType;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].vidBuf+t*1400,1400);
						bytes=1400+14;						
						sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
					}
				}
			} 
#if 0
			timestamp_increse = 8000;
			memset(sendbuf,0,sizeof(sendbuf));
			nAvFrmLen = g_rtpPack[nChanNum].nAudLen;
			
			rtp_hdr =(RTP_FIXED_HEADER*)&sendbuf[0];
			rtp_hdr->payload     = RTP_G726;  
			rtp_hdr->version     = 2;        
			rtp_hdr->marker    = 0;          
			rtp_hdr->ssrc      = htonl(10);  
			
			if(nAvFrmLen<=1400) {
				rtp_hdr->marker=0;
				rtp_hdr->seq_no     = htons(g_rtspClients[i].seqnum++);
				nalu_hdr =(NALU_HEADER*)&sendbuf[12]; 
				nalu_hdr->F=0; 
				nalu_hdr->NRI=  nIsIFrm; 
				nalu_hdr->TYPE=  nNaluType;
				
				nalu_payload=&sendbuf[13];
				memcpy(nalu_payload,g_rtpPack[nChanNum].audBuf,g_rtpPack[nChanNum].nAudLen);
				g_rtspClients[i].tsaud=g_rtspClients[i].tsaud+timestamp_increse;
				
				rtp_hdr->timestamp=htonl(g_rtspClients[i].tsaud);
				bytes=g_rtpPack[nChanNum].nAudLen + 13 ;				
				sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
			} else if(nAvFrmLen>1400) {
				printf("-------->1400-----------------------%d\n",nAvFrmLen);
				int k=0,l=0;
				k=nAvFrmLen/1400;
				l=nAvFrmLen%1400;
				int t=0;         
				g_rtspClients[i].tsaud=g_rtspClients[i].tsaud+timestamp_increse;
				rtp_hdr->timestamp=htonl(g_rtspClients[i].tsaud);
				while(t<=k) {
					rtp_hdr->seq_no = htons(g_rtspClients[i].seqnum++); 
					if(t==0) {
						rtp_hdr->marker=0;
						fu_ind =(FU_INDICATOR*)&sendbuf[12];
						fu_ind->F= 0;   
						fu_ind->NRI= nIsIFrm; 
						fu_ind->TYPE=28;
						
						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->E=0;
						fu_hdr->R=0;
						fu_hdr->S=1;
						fu_hdr->TYPE=nNaluType;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].audBuf,1400);
						
						bytes=1400+14;						
						sendto( udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
					} else if(k==t) {
						rtp_hdr->marker=1;
						fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
						fu_ind->F= 0 ;   
						fu_ind->NRI= nIsIFrm ;  
						fu_ind->TYPE=28;
						
						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->R=0;
						fu_hdr->S=0;
						fu_hdr->TYPE= nNaluType;
						fu_hdr->E=1;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].audBuf+t*1400,l);
						bytes=l+14;		
						sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
					} else if(t<k && t!=0) {
						rtp_hdr->marker=0;
						fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
						fu_ind->F=0;  
						fu_ind->NRI=nIsIFrm;
						fu_ind->TYPE=28;

						fu_hdr =(FU_HEADER*)&sendbuf[13];
						fu_hdr->R=0;
						fu_hdr->S=0;
						fu_hdr->E=0;
						fu_hdr->TYPE=nNaluType;
						
						nalu_payload=&sendbuf[14];
						memcpy(nalu_payload,g_rtpPack[nChanNum].audBuf+t*1400,1400);
						bytes=1400+14;						
						sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
						t++;
					}
				}
			} 
#endif			
			g_rtpPack[nChanNum].bIsFree = TRUE;
		}
	}
	return NULL;
}

void InitRtspServer()
{
	int i;
	pthread_t threadId = 0;
	for(i=0;i<MAX_RTSP_CHAN;i++)
	{
		memset(&g_rtpPack[i],0,sizeof(RTSP_PACK));
		g_rtpPack[i].bIsFree = TRUE;
		//g_rtpPack.bWaitIFrm = TRUE;
		memset(&g_FrmPack[i],0,sizeof(FRAME_PACK));
	}
	pthread_mutex_init(&g_sendmutex,NULL);
	pthread_mutex_init(&g_mutex,NULL);
	pthread_cond_init(&g_cond,NULL);
	memset(g_rtspClients,0,sizeof(RTSP_CLIENT)*MAX_RTSP_CLIENT);
	
	pthread_create(&g_SendDataThreadId, NULL, SendDataThread, NULL);
	pthread_create(&threadId, NULL, RtspServerListen, NULL);
	printf("RTSP:-----Init Rtsp server\n");
}


int AddFrameToRtspBuf(int nChanNum, enMonBlockType eType, LPBYTE pData, UINT nSize, UINT nVidFrmNum, BOOL bIFrm) {
	if(eType == MBT_VIDEO) {
		if(g_FrmPack[nChanNum].nFrameID!= nVidFrmNum && nVidFrmNum != 0) {
			memcpy(g_rtpPack[nChanNum].vidBuf,g_FrmPack[nChanNum].vidBuf,g_FrmPack[nChanNum].vidLen);
			memcpy(g_rtpPack[nChanNum].audBuf,g_FrmPack[nChanNum].audBuf,g_FrmPack[nChanNum].audLen);
			g_rtpPack[nChanNum].nVidLen = g_FrmPack[nChanNum].vidLen;
			g_rtpPack[nChanNum].nAudLen = g_FrmPack[nChanNum].audLen;
			g_rtpPack[nChanNum].bIsIFrm = bIFrm;
			g_rtpPack[nChanNum].bIsFree = FALSE;
			g_nSendDataChn = nChanNum;
			pthread_mutex_lock(&g_mutex);
			pthread_cond_signal(&g_cond);
			pthread_mutex_unlock(&g_mutex);
			
			if(nSize < RTSP_MAX_VID) {
				memcpy(g_FrmPack[nChanNum].vidBuf,pData,nSize);
				g_FrmPack[nChanNum].vidLen = nSize;   
				g_FrmPack[nChanNum].nFrameID= nVidFrmNum;   
				g_FrmPack[nChanNum].audLen = 0;
			}
		} else {
			if(g_FrmPack[nChanNum].vidLen+nSize < RTSP_MAX_VID) {
				memcpy(g_FrmPack[nChanNum].vidBuf+g_FrmPack[nChanNum].vidLen,pData,nSize);
				g_FrmPack[nChanNum].vidLen += nSize;
			} else {
				printf("rtsp max vid frame !!!\n");
			}
		}
    } else {
		if(g_FrmPack[nChanNum].audLen+nSize < RTSP_MAX_AUD) {
			memcpy(g_FrmPack[nChanNum].audBuf+g_FrmPack[nChanNum].audLen,pData,nSize);
			g_FrmPack[nChanNum].audLen += nSize;
		} else {
			g_FrmPack[nChanNum].audLen = 0;
		}
	}
	
	return 0;
}







