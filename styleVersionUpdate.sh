#!/bin/bash

gzip=`which gzip`
STYLE_DOMAIN="http://172.22.9.69"
DOWNLOAD_DIR="${forum_output}"
STYLE_VERSION_URI="styleVersion" 

#default 2 minutes
interval="2m"

lastModified=""
httpStatus=0

STYLE_VERSION_URL="${STYLE_DOMAIN}/${STYLE_VERSION_URI}.gz" 
DOWNLOAD_DIR_FILE="${DOWNLOAD_DIR}/${STYLE_VERSION_URI}.gz"
DOWNLOAD_DIR_FILE_TMP="${DOWNLOAD_DIR}/${STYLE_VERSION_URI}.gz_tmp"
UNGZIP_DIR_FILE="${DOWNLOAD_DIR}/${STYLE_VERSION_URI}"
RESPONSE_LOG="${DOWNLOAD_DIR}/response"

getLastModified() {
	responseLog=$1
	lastModified=""
    if [ -f "$responseLog" ]; then
        lastModified=`cat $responseLog |grep "Last-Modified:" | awk -F"Last-Modified:" '{print $2}'`
    fi
}

getHttpStatus() {
	responseLog=$1
	httpStatus=0
    if [ -f "$responseLog" ]; then
        httpStatus=`cat $responseLog |grep "HTTP/" | awk '{print $2}'`
        if [ -z "$httpStatus" ]; then
        	httpStatus=0
        fi
    fi
}

ungzip() {
    `$gzip -cd $1 > $2`
}

checkAndUngzip() {
	
	getHttpStatus $RESPONSE_LOG
        
    ##������ص�������html�������ʾ���������汾�ļ��滻��������سɹ�grep �������ļ���ᱨ���������-vȥ������
    isHtml=`cat ${DOWNLOAD_DIR_FILE_TMP} | head -10| grep "<" | grep -v "Binary file"`

	##http״̬����200��ʾ��ȷ�������304�����������κδ���    
    if [ $httpStatus -eq 200 ] && [ -z "$isHtml" ]; then
    	## ��tmp�ļ��޸ĳɿ����ļ�
        mv ${DOWNLOAD_DIR_FILE_TMP} ${DOWNLOAD_DIR_FILE}
        
        ##���ļ���ѹ�� ${UNGZIP_DIR_FILE} Ŀ¼�ļ���
        ungzip ${DOWNLOAD_DIR_FILE} ${UNGZIP_DIR_FILE}
    fi
}

download() {
	
	getHttpStatus $RESPONSE_LOG
	##ֻҪ����304��ȥ��ȡ�µ�lastModified,����lastModified��Զ���ᱻ�޸ĵ�
	if [ $httpStatus -ne 304 ]; then
		getLastModified $RESPONSE_LOG	
	fi
    
	##�����lastModified��Ϣ�������ͷ��Ϣȥ����
	param=""
    if [ -n "$lastModified" ]; then
    	param="--header=If-Modified-Since:${lastModified}"
    fi
    ##���ذ汾�ļ�
    wget -S -t 0 -T 5 "$param" ${STYLE_VERSION_URL} -O ${DOWNLOAD_DIR_FILE_TMP} > $RESPONSE_LOG 2>&1
    checkAndUngzip
}

main() {
    mkdir -p $DOWNLOAD_DIR
    if [ -f "${UNGZIP_DIR_FILE}" ]; then
    	rm ${UNGZIP_DIR_FILE}*
    fi
    if [ -f "$RESPONSE_LOG" ]; then
    	rm $RESPONSE_LOG
    fi
    
    while [ true ]; do
        ##���ص���
        download
        sleep $interval
    done
}

main
