/**
 * zhiwen.mizw@alibaba-inc.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <ctype.h>
#include <sys/stat.h>
#include <malloc.h>

#include "httpd.h"
#include "apr_buckets.h"
#include "http_config.h"
#include "http_protocol.h"
#include "ap_config.h"
#include "http_log.h"
#include "apr_strings.h"
#include "util_filter.h"
#include "http_request.h"
#include "apr_hash.h"

#define STYLE_COMBINE_NAME "styleCombine"

module AP_MODULE_DECLARE_DATA styleCombine_module;

#define MODULE_BRAND "styleCombine/1.0.1"
#define EXT_JS ".js"
#define EXT_CSS ".css"
#define URI_SEPARATOR "|"
#define POSITION_TOP "top"
#define POSITION_HEAD "head"
#define POSITION_FOOTER "footer"
#define DEBUG_MODE "_debugMode_="
#define RUN_MODE_STATUS "dis"
#define JS_TAG_EXT_PREFIX_TXT "<script type=\"text/javascript\" src=\""
#define JS_TAG_EXT_SUFFIX_TXT "\"></script>"
#define JS_TAG_PREFIX_TXT "<script type=\"text/javascript\">"
#define JS_TAG_SUFFIX_TXT "</script>"
#define CSS_PREFIX_TXT "<link rel=\"stylesheet\" href=\""
#define CSS_SUFFIX_TXT "\" />"

#define BUFFER_PIECE_SIZE 128
#define DEFAULT_CONTENT_LEN (1024 << 10)// default html content size 1M
#define DOMAIN_COUNTS 2

int JS_TAG_PREFIX_LEN = 0, JS_TAG_SUFFIX_LEN = 0;
int JS_TAG_EXT_PREFIX_LEN = 0, JS_TAG_EXT_SUFFIX_LEN = 0;
int CSS_PREFIX_LEN = 0, CSS_SUFFIX_LEN = 0;
int DEBUG_MODE_LEN = 12;

/***********global variable************/
const char ZERO_END = '\0';
/*position char */
enum PositionEnum{TOP, HEAD, FOOTER, NONE};

int styleTableSize = 40000;

apr_pool_t   *globalPool = NULL;
apr_table_t  *styleVersionTable = NULL;
apr_time_t    lastLoadTime;
__time_t      prevTime = 0;
char         *appRunMode = NULL;
server_rec   *server;

typedef struct {
	char *ptr;
	off_t used;
	off_t size;
} buffer;

typedef struct {
	buffer *prefix;
	buffer *mark;
	buffer *refTag;
	buffer *closeTag;
	char    suffix;
	/*0:表示css; 1:表示js*/
	int    styleType;
} ParserTag;

ParserTag             *cssPtag;
ParserTag             *jsPtag;

typedef struct {
	int        enabled;
	char      *filterCntType;
	buffer    *oldDomains[DOMAIN_COUNTS];
	buffer    *newDomains[DOMAIN_COUNTS];
	char      *versionFilePath;
	int        maxUrlLen;
	int        printLog;
	char      *appName;
} CombineConfig;

typedef struct {
	int                 debugMode;
	buffer             *buf;
    apr_bucket_brigade *pbbOut;
} CombineCtx;

typedef struct {
	buffer               *styleUri;
	off_t                 async;
	buffer               *group;
	enum PositionEnum     postion;
	buffer               *version;
	off_t                 styleType;
	off_t                 domainIndex;
	off_t                 isCombined;
} StyleField;

typedef struct ListNode ListNode;
struct ListNode {
	ListNode   *next;
    const void *value;
};
typedef struct {
	off_t                 size;
	ListNode             *first;
	ListNode             *head;
} LinkedList;

typedef struct {
	buffer *topBuf;
	buffer *headBuf;
	buffer *footerBuf;
} CombinedStyle;

typedef struct {
	buffer          *domain;
	buffer          *styleUri;
	buffer          *version;
	buffer          *group;
	off_t            async;
	unsigned int     isNewLine;
	unsigned int     styleType;
	unsigned int     needExt;
} TagConfig;

void fillTagConfigParams(TagConfig *tagConfig, buffer *version,
		int isNewLine, int styleType, int needExt) {
	tagConfig->version = version;
	tagConfig->isNewLine = isNewLine;
	tagConfig->styleType = styleType;
	tagConfig->needExt = needExt;
}

static void printf_log(buffer *buf, char *str) {
//	char strBuf[10240];
//	memset(strBuf,0,10240);
//	sprintf(&strBuf,str,buf,buf->ptr,buf->size,buf->used);
//	ap_log_error(APLOG_MARK, APLOG_ERR, 0, server, "%s", strBuf);
	return ;
}

StyleField *style_field_create() {
	StyleField *styleField = malloc(sizeof(StyleField));
	if(NULL == styleField) {
		return NULL;
	}
	styleField->async = 0;
	styleField->styleUri = NULL;
	styleField->version = NULL;
	styleField->postion = NONE;
	styleField->styleType = 0;
	styleField->domainIndex = 0;
	styleField->isCombined = 0;
	styleField->group = NULL;
	return styleField;
}

buffer *buffer_init() {
	buffer *buf = malloc(sizeof(buffer));
	if(NULL == buf) {
		return buf;
	}
	buf->ptr = NULL;
	buf->used = 0;
	buf->size = 0;
	printf_log(buf, "buffer_init: buf=%p  ptr=%p size=%ld used=%ld");
	return buf;
}

void buffer_free(buffer *b) {
	if(NULL == b) {
		return;
	}
	printf_log(b, "buffer_free: buf=%p  ptr=%p size=%ld used=%ld");
	free(b->ptr);
	free(b);
	return;
}

void style_field_free(StyleField *styleField) {
	if(NULL == styleField) {
		return;
	}
	buffer_free(styleField->version);
	buffer_free(styleField->group);
	buffer_free(styleField->styleUri);
	free(styleField);
	return;
}

buffer *buffer_init_size(int size) {
	buffer *buf = buffer_init();
	if(NULL == buf) {
		return buf;
	}
	buf->ptr = malloc(size);
	if(NULL == buf->ptr) {
		buffer_free(buf);
		return NULL;
	}
	buf->size = size;
	printf_log(buf, "buffer_init_size: buf=%p  ptr=%p size=%ld used=%ld");
	return buf;
}

void add(LinkedList *list, void *item) {
	if (NULL == list || NULL == item) {
		return;
	}
	ListNode *node = malloc(sizeof(ListNode));
	if(NULL == node) {
		return;
	}
	node->next = NULL;
	node->value = item;

	if (NULL == list->first) {
		list->first = node;
		list->size = 0;
	} else {
		list->head->next = node;
	}
	++list->size;
	list->head = node;
}

void linkedList_free(LinkedList *list) {
	if (NULL == list) {
		return;
	}
	ListNode *node = list->first;
	while(NULL != node) {
		ListNode *nextNode = (ListNode *)node->next;
		style_field_free((StyleField *)node->value);
		free(node);
		node = nextNode;
	}
	free(list);
}

void buffer_clean(buffer *buf) {
	if(NULL == buf) {
		return;
	}
	buf->used = 0;
	buf->ptr[0]=ZERO_END;
}

void combinedStyle_clean(CombinedStyle *c) {
	if(NULL == c) {
		return;
	}
	buffer_clean(c->footerBuf); buffer_clean(c->headBuf); buffer_clean(c->topBuf);
	return;
}
void combinedStyle_free(CombinedStyle *c) {
	if(NULL == c) {
		return;
	}
	buffer_free(c->footerBuf); buffer_free(c->headBuf);	buffer_free(c->topBuf);
	return;
}

/**
 * get uri extention
 */
static char *getFileExt(char *uri, int len) {
	if (NULL == uri) {
		return NULL;
	}
	if (0 == memcmp(EXT_JS, uri + len - 3, 3)) {
		return EXT_JS;
	}
	if (0 == memcmp(EXT_CSS, uri + len - 4, 4)) {
		return EXT_CSS;
	}
	return NULL;
}

static void stringAppend(buffer *buf, char *str, int strLen) {
	if(NULL == buf || NULL == str || strLen <= 0) {
		return;
	}
	if(0 == buf->size) {
		if(strLen > BUFFER_PIECE_SIZE) {
			buf->size = strLen + BUFFER_PIECE_SIZE;
		} else {
			buf->size = BUFFER_PIECE_SIZE;
		}
		buf->ptr = malloc(buf->size);
		buf->used = 0;
		printf_log(buf, "stringAppend malloc: buf=%p  ptr=%p size=%ld used=%ld");
	}
	if(buf->used + strLen >= buf->size) {
		printf_log(buf, "stringAppend old realloc: buf=%p  ptr=%p size=%ld used=%ld");
		buf->size += (strLen + BUFFER_PIECE_SIZE);
		buf->ptr = realloc(buf->ptr, buf->size);
		printf_log(buf, "stringAppend new realloc: buf=%p  ptr=%p size=%ld used=%ld");
	}
	memcpy(buf->ptr + buf->used, str, strLen);
	buf->used += strLen;
	buf->ptr[buf->used] = ZERO_END;
	return;
}

static void formatParser(apr_table_t *table, char *str, server_rec *server) {
	if (NULL == str || NULL == table) {
		return;
	}
	char *name = NULL, *value = NULL;
	char *srcStr = str;
	char *strLine = NULL;
	while (NULL != (strLine = strsep(&srcStr, "\n"))) {
		name = strsep(&strLine, "=");
		if (NULL == name || strlen(name) <= 1) {
			continue;
		}
		value = strLine;
		if (NULL == value || strlen(value) < 1) {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, server,
					"formatParser value error value=[%s],strLine=[%s]", value, strLine);
			continue;
		}
		apr_table_set(table, name, value);
	}
	return;
}

time_t getURIVersion(buffer *styleUri, char *singleUri, request_rec *r, CombineConfig *pConfig) {
	if(NULL == styleUri || NULL == singleUri) {
		return 0;
	}

	time_t newVersion = 0;
	char *fileExt = getFileExt(styleUri->ptr, styleUri->used);
	if (NULL == fileExt) {
		return 0;
	}
	int uriLen = styleUri->used;

	if(NULL == styleVersionTable) {
		time(&newVersion);
		newVersion = newVersion / 600;

		styleUri->ptr[styleUri->used] = ZERO_END;
		// add log
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
		  "=method=getURIVersion err, urls:[%s] requestURI:[%s]", styleUri->ptr, r->unparsed_uri);

		return newVersion;
	}

	int i , t = 0;
	for(i = 0; i < uriLen; ++i, ++t) {
		if(i != 0 && 0 == memcmp(&styleUri->ptr[i], URI_SEPARATOR, 1)) {
			singleUri[t] = ZERO_END;
		} else {
			singleUri[t] = styleUri->ptr[i];
			if((i + 1) != uriLen) {
				continue;
			}
			singleUri[++t] = ZERO_END;
		}
		if (NULL == getFileExt(singleUri, t)) {
			memcpy(singleUri + t, fileExt, strlen(fileExt));
		}
		t = -1;
		if(NULL != styleVersionTable) {
			const char *strVs = apr_table_get(styleVersionTable, singleUri);
			if(NULL != strVs) {
				newVersion += atol(strVs);
			} else {
				// add log
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
				"==method=getURIVersion err, uris[%s]  uri:[%s]  requestURI:[%s]", styleUri->ptr, singleUri, r->unparsed_uri);
			}
		}
	}
	if (newVersion <= 0) {
		time(&newVersion);
		newVersion = newVersion / 600;
	}
	return newVersion;
}

buffer *getStrVersion(buffer *styleUri, char *singleUri, request_rec *r, CombineConfig *pConfig){
	//getURIVersion(styleUri, singleUri, r, pConfig);
	buffer *buf =  buffer_init_size(10);
	stringAppend(buf, "1234", 4);
	return buf;
}

static enum PositionEnum strToPosition(char *str, int **posLen) {
	if(NULL == str) {
		return NONE;
	}
	if (0 == strncmp(POSITION_TOP, str, 3)) {
		*posLen = (int *)3;
		return TOP;
	}
	if (0 == strncmp(POSITION_HEAD, str, 4)) {
		*posLen = (int *)4;
		return HEAD;
	}
	if (0 == strncmp(POSITION_FOOTER, str, 6)) {
		*posLen = (int *)6;
		return FOOTER;
	}
	*posLen = (int *)0;
	return NONE;
}

static void loadStyleVersion(request_rec *r, CombineConfig *pConfig) {

	if(NULL == pConfig) {
		return;
	}
	apr_pool_t *pool = r->pool;
	apr_finfo_t finfo;
	apr_file_t *fd = NULL;
	apr_size_t amt;
	int intervalSecd = 20;
	struct timeval tv;
	if(NULL == pConfig->versionFilePath) {
		return;
	}
	gettimeofday(&tv, NULL);
	if(0 != prevTime && (tv.tv_sec - prevTime) <= intervalSecd) {
		return;
	}
	prevTime = tv.tv_sec;
	apr_status_t rc = apr_stat(&finfo, pConfig->versionFilePath, APR_FINFO_MIN, pool);

	if(APR_SUCCESS == rc && finfo.mtime != lastLoadTime) {
		if(5 == pConfig->printLog) {
			ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
					"==reload styleVersion File lastLoadTime: %ld == fmtime:%ld", lastLoadTime, finfo.mtime);
		}
		lastLoadTime = finfo.mtime;
		rc = apr_file_open(&fd, pConfig->versionFilePath, APR_READ | APR_BINARY | APR_XTHREAD,
						   APR_OS_DEFAULT, pool);
		if(rc != APR_SUCCESS) {
			apr_file_close(fd);
		 	return;
		}
		amt = (apr_size_t)finfo.size;
		apr_table_t  *newTable = NULL;
		char         *versionBuf = apr_pcalloc(pool, amt + 1);

		if(NULL == versionBuf) {
			apr_file_close(fd);
			return;
		}
		rc = apr_file_read(fd, versionBuf, &amt);
		if(APR_SUCCESS == rc) {

			apr_pool_t *newPool = NULL;
			apr_pool_create(&newPool, r->server->process->pool);
			if(NULL == newPool) {
				apr_file_close(fd);
				return ;
			}
			newTable = apr_table_make(newPool, styleTableSize);
			formatParser(newTable, versionBuf, r->server);
			styleVersionTable = newTable;
			apr_pool_t *oldPool = globalPool;
			globalPool = newPool;
			//free old pool
			if(NULL != oldPool) {
				apr_pool_destroy(oldPool);
			}
			appRunMode = apr_table_get(styleVersionTable, pConfig->appName);
		}
		apr_file_close(fd);
	}
	return;
}

static StyleField *tagParser(CombineConfig *pConfig, ParserTag *ptag, char *maxTagBuf, off_t maxTagLen) {
	if(NULL == pConfig || NULL == ptag || NULL == maxTagBuf) {
		return NULL;
	}
	//domain find & checker
	off_t tagLen = ptag->prefix->used + ptag->refTag->used;
	if(maxTagLen <= (pConfig->oldDomains[0]->used + (tagLen + 1))){
		return NULL;
	}
	int dIndex = 0;
	char *currURL = NULL,*tmpMaxTagBuf = maxTagBuf + tagLen;
	for(dIndex = 0; dIndex < DOMAIN_COUNTS; dIndex++) {
		buffer *domain = pConfig->oldDomains[dIndex];
		if(NULL == domain) {
			continue;
		}
		char *urlStart = strstr(tmpMaxTagBuf, pConfig->oldDomains[dIndex]->ptr);
		if(NULL != urlStart) {
			currURL = urlStart;
			break;
		}
	}
	//对于没有域名的css/js不进行处理
	if (NULL == currURL) {
		return NULL;
	}

	//style mark checker only css
	if(0 == ptag->styleType) {
		tmpMaxTagBuf = maxTagBuf + ptag->prefix->used + 5; // +5 = {' rel='}
		if (NULL == strstr(tmpMaxTagBuf, ptag->mark->ptr)) {
			return NULL;
		}
	}

	char *currURI = currURL + pConfig->oldDomains[dIndex]->used;
	register int len = 0, hasDo = 0, stop = 0;
	//min len <script src="">
	buffer *styleUri = buffer_init_size(maxTagLen - pConfig->oldDomains[dIndex]->used - 15);
	while(*currURI && len < pConfig->maxUrlLen) {
		if(*currURI == '"' || *currURI == '\'' || *currURI == ptag->suffix) {
			break;
		}
		++len;
		if(isspace(*currURI)) {
			continue;
		}
		switch(*currURI) {
		case '.':
			++hasDo;
			break;
		case '?':
			//清除uri后面带的参数
			stop = 1;
			break;
		default:
			break;
		}
		if(stop) {
			break;
		}
		styleUri->ptr[styleUri->used++] = *currURI;
		++currURI;
	}
	if (!hasDo) { //no .js/.css ext
		buffer_free(styleUri);
		return NULL;
	}
	styleUri->ptr[styleUri->used] = ZERO_END;

	StyleField *styleField = style_field_create();
	if(NULL == styleField) {
		buffer_free(styleUri);
		return NULL;
	}
	// get pos & async
	off_t fieldCount = 0;
	enum PositionEnum position = NONE;
	buffer *group = NULL;
	tmpMaxTagBuf = maxTagBuf + ptag->prefix->used;
	while(*tmpMaxTagBuf) {
		if(isspace(*tmpMaxTagBuf)) {
			++tmpMaxTagBuf;
			switch(*tmpMaxTagBuf) {
			case 'p':
				if(0 == memcmp(++tmpMaxTagBuf, "os=", 3)) {
					int *posLen = (int *) 0;
					position = strToPosition((tmpMaxTagBuf += 4), &posLen);
					tmpMaxTagBuf += (int) posLen;
				}
				break;
			case 'a':
				if(0 == memcmp(++tmpMaxTagBuf, "sync=", 5)) {
					tmpMaxTagBuf += 6;
					if(0 == memcmp(tmpMaxTagBuf, "true", 4)) {
						styleField->async = 1;
						tmpMaxTagBuf += 4;
					}
				}
				break;
			case 'g':
				if(0 == memcmp(++tmpMaxTagBuf, "roup=", 5)) {
					int len = 0;
					tmpMaxTagBuf += 6;
					char *s = tmpMaxTagBuf;
					while(*s) {
						switch(*s) {
						case '\'':
							stop = 1;
							break;
						case '"':
							stop = 1;
							break;
						}
						if(stop) {
							break;
						}
						++s;
						++len;
					}
					group = buffer_init_size(100);
					stringAppend(group, tmpMaxTagBuf, len);
					tmpMaxTagBuf += len;
				}
				break;
			default:
				//处理多空格情况下的问题
				--tmpMaxTagBuf;
				break;
			}
		}
		++tmpMaxTagBuf;
	}

	if(NONE != position && NULL == group) {
		//create default group
		group = buffer_init_size(20);
		stringAppend(group, "_def_group_name_", 16);
		char posIndex[1];
		sprintf(posIndex, "%d", position);
		group->ptr[group->used++] = posIndex[0];
		group->ptr[group->used] = ZERO_END;
	}
	styleField->domainIndex = dIndex;
	styleField->group = group;
	styleField->styleType = ptag->styleType;
	styleField->postion = position;
	styleField->styleUri = styleUri;
	return styleField;
}

static void addExtStyle(buffer *destBuf, TagConfig *tagConfig) {
	if(NULL == destBuf || NULL == tagConfig
			|| NULL == tagConfig->styleUri || !tagConfig->styleUri->used) {
		return ;
	}
	if (tagConfig->isNewLine) {
		stringAppend(destBuf, "\n", 1);
	}
	if (tagConfig->styleType) {
		stringAppend(destBuf, JS_TAG_EXT_PREFIX_TXT, JS_TAG_EXT_PREFIX_LEN);
	} else {
		stringAppend(destBuf, CSS_PREFIX_TXT, CSS_PREFIX_LEN);
	}
	stringAppend(destBuf, tagConfig->domain->ptr, tagConfig->domain->used);
	stringAppend(destBuf, tagConfig->styleUri->ptr, tagConfig->styleUri->used);
	//append ext
	if(tagConfig->needExt) {
		//append the version ext
		if (tagConfig->styleType) {
			stringAppend(destBuf, EXT_JS, 3);
		} else {
			stringAppend(destBuf, EXT_CSS, 4);
		}
	}
	//append version
	stringAppend(destBuf, "?_v=", 4);
	stringAppend(destBuf, tagConfig->version->ptr, tagConfig->version->used);
	//append the version ext
	if (tagConfig->styleType) {
		stringAppend(destBuf, EXT_JS, 3);
		stringAppend(destBuf, JS_TAG_EXT_SUFFIX_TXT, JS_TAG_EXT_SUFFIX_LEN);
	} else {
		stringAppend(destBuf, EXT_CSS, 4);
		stringAppend(destBuf, CSS_SUFFIX_TXT, CSS_SUFFIX_LEN);
	}
	return;
}

/**
 * 将js/css列表合并成一个url,并放到相应的位置上去
 */
static void combineStyles(CombineConfig *pConfig, TagConfig *tagConfig, LinkedList *styleList,
								CombinedStyle *combinedStyle, CombinedStyle *tmpCombine, CombinedStyle *styleVersion) {
	if(NULL == styleList) {
		return;
	}
	StyleField *styleField = NULL;
	ListNode *node = styleList->first;
	if(NULL == node || NULL == (styleField = (StyleField *)node->value)) {
		return;
	}
	register int top = 0, head = 0, footer = 0;
	register buffer *tmpUriBuf = NULL;

	tagConfig->styleType = styleField->styleType;
	tagConfig->domain  = pConfig->newDomains[styleField->domainIndex];
	while(NULL != node) {
		styleField = (StyleField *) node->value;
		switch(styleField->postion) {
			case TOP: //top
				stringAppend(styleVersion->topBuf, styleField->version->ptr, styleField->version->used);
				tmpUriBuf = tmpCombine->topBuf;
				if (top > 0) {
					stringAppend(tmpUriBuf, URI_SEPARATOR, 1);
					break;
				}
				top = 1;
				break;
			case HEAD: //head
				stringAppend(styleVersion->headBuf, styleField->version->ptr, styleField->version->used);
				tmpUriBuf = tmpCombine->headBuf;
				if (head > 0) {
					stringAppend(tmpUriBuf, URI_SEPARATOR, 1);
					break;
				}
				head = 1;
				break;
			case FOOTER: //footer
				stringAppend(styleVersion->footerBuf, styleField->version->ptr, styleField->version->used);
				tmpUriBuf = tmpCombine->footerBuf;
				if (footer > 0) {
					stringAppend(tmpUriBuf, URI_SEPARATOR, 1);
					break;
				}
				footer = 1;
				break;
			default:
				break;
		}
		fillTagConfigParams(tagConfig, NULL, 1, tagConfig->styleType, 1);
		//url拼接在一起的长度超过配置的长度，则需要分成多个请求来处理。
		int urlLen = (tagConfig->domain->used + tmpUriBuf->used + styleField->styleUri->used);
		if (urlLen >= pConfig->maxUrlLen) {
			//将合并的url最后一个|字符去除
			tmpUriBuf->ptr[--tmpUriBuf->used] = ZERO_END;
			tagConfig->styleUri = tmpUriBuf;
			switch(styleField->postion) {
				case TOP:
					tagConfig->version = styleVersion->topBuf;
					addExtStyle(combinedStyle->topBuf, tagConfig);
					buffer_clean(styleVersion->topBuf);
					break;
				case HEAD:
					tagConfig->version = styleVersion->headBuf;
					addExtStyle(combinedStyle->headBuf, tagConfig);
					buffer_clean(styleVersion->headBuf);
					break;
				case FOOTER:
					tagConfig->version = styleVersion->footerBuf;
					addExtStyle(combinedStyle->footerBuf, tagConfig);
					buffer_clean(styleVersion->footerBuf);
					break;
				default:
					break;
			}
			//reset value
			tmpUriBuf->used = 0;
		}
		if(tagConfig->styleType) {
			styleField->styleUri->used -= 3; // clean .js ext
		} else {
			styleField->styleUri->used -= 4; // clean .css ext
		}
		stringAppend(tmpUriBuf, styleField->styleUri->ptr, styleField->styleUri->used);
		node = node->next;
	}

	tagConfig->version = styleVersion->topBuf;
	tagConfig->styleUri = tmpCombine->topBuf;
	addExtStyle(combinedStyle->topBuf, tagConfig);

	tagConfig->version = styleVersion->headBuf;
	tagConfig->styleUri = tmpCombine->headBuf;
	addExtStyle(combinedStyle->headBuf, tagConfig);

	tagConfig->version = styleVersion->footerBuf;
	tagConfig->styleUri = tmpCombine->footerBuf;
	addExtStyle(combinedStyle->footerBuf, tagConfig);
	return;
}

/**
 * 用于开发时，打开调试模块调用。将js/css的位置做移动，但不做合并
 */
static void combineStylesDebug(CombineConfig *pConfig, TagConfig *tagConfig, LinkedList *styleList,
								CombinedStyle *combinedStyle) {
	if(NULL == styleList) {
		return;
	}
	StyleField *styleField = NULL;
	ListNode *node = styleList->first;
	if(NULL == node || NULL == (styleField = (StyleField *)node->value)) {
		return;
	}
	tagConfig->styleType = styleField->styleType;
	tagConfig->domain = pConfig->newDomains[styleField->domainIndex];
	tagConfig->isNewLine = 1, tagConfig->needExt = 0;

	while(NULL != node) {
		styleField = (StyleField *) node->value;
		tagConfig->version = styleField->version;
		tagConfig->styleUri = styleField->styleUri;
		switch (styleField->postion) {
			case TOP: //top
				addExtStyle(combinedStyle->topBuf, tagConfig);
				break;
			case HEAD: //head
				addExtStyle(combinedStyle->headBuf, tagConfig);
				break;
			case FOOTER: //footer
				addExtStyle(combinedStyle->footerBuf, tagConfig);
				break;
			default:
				break;
		}
		node = node->next;
	}
	return;
}

static int isRepeat(request_rec *r, apr_hash_t *duplicats, StyleField *styleField) {
	if(NULL == duplicats) {
		return 0;
	}
	//add domain area
	char domainIndex[1];
	sprintf(domainIndex, "%ld", styleField->domainIndex);
	//make a key
	char *key = apr_palloc(r->pool, styleField->styleUri->used + 2);
	if(NULL == key) {
		return 0;
	}
	off_t keylen = 0;
	key[keylen++] = domainIndex[0];
	memcpy((key + keylen), styleField->styleUri->ptr, styleField->styleUri->used);
	key[keylen += styleField->styleUri->used] = ZERO_END;

	if(NULL != apr_hash_get(duplicats, key, keylen)) {
		//if uri has exsit then skiping it
		return 1;
	}
	apr_hash_set(duplicats, key, keylen, "1");
	return 0;
}

static void addBucket(conn_rec *c, apr_bucket_brigade *pbbkOut, char *str, int strLen) {
	if(NULL == str || strLen <= 0) {
		return;
	}
	apr_bucket *pbktOut = NULL;
	pbktOut = apr_bucket_heap_create(str, strLen, NULL, c->bucket_alloc);
	APR_BRIGADE_INSERT_TAIL(pbbkOut, pbktOut);
	return;
}
static void resetHtml(conn_rec *c, apr_bucket_brigade *pbbkOut,
						CombinedStyle *combinedStyle, buffer *buf) {
	if(NULL== buf || NULL == buf->ptr) {
		return;
	}
	char *sourceHtml = buf->ptr;

	int index = 0;
	char *headIndex = strstr(sourceHtml, "</head>");
	if(NULL != headIndex) {
		addBucket(c, pbbkOut, sourceHtml, (index = headIndex - sourceHtml));
	}
	addBucket(c, pbbkOut, combinedStyle->topBuf->ptr, combinedStyle->topBuf->used);
	addBucket(c, pbbkOut, combinedStyle->headBuf->ptr, combinedStyle->headBuf->used);

	char *endHtmlIndex = buf->ptr + buf->used;
	char *middleIndex = (sourceHtml + index);
	char *footerIndex = strstr(middleIndex, "</body>");
	if(NULL != footerIndex) {
		addBucket(c, pbbkOut, middleIndex, (footerIndex - middleIndex));
		addBucket(c, pbbkOut, combinedStyle->footerBuf->ptr, combinedStyle->footerBuf->used);
		addBucket(c, pbbkOut, footerIndex, (endHtmlIndex - footerIndex));
	} else {
		addBucket(c, pbbkOut, middleIndex, (endHtmlIndex - middleIndex));
		addBucket(c, pbbkOut, combinedStyle->footerBuf->ptr, combinedStyle->footerBuf->used);
	}
	return;
}

static inline char *strSearch(const char * str1, int **matchedType, int **isExpression) {
	register char *cp = (char *) str1;
	register char *s1 = NULL;

	register int r = -1;
	while (*cp) {
		//compare first
		if ('<' == *cp) {
			s1 = cp;
			s1++;
			switch (*s1) {
				case 's': //script
					r = memcmp("cript", ++s1, 5);
					*matchedType = (int *) 1; //js
					break;
				case 'l': //link
					r = memcmp("ink", ++s1, 3);
					*matchedType = (int *) 0; //css
					break;
				case '!':
					if (0 == memcmp("--", ++s1, 2)) {
						if ('[' == *(s1 + 2)) {
							cp += 12; //skip "<!--[if IE]>"
							*isExpression = (int *) 1;
							continue;
						}
						//skip comments "<!--xxxxx -->"
						for (; *cp != '\0'; cp++) {
							if (0 == memcmp(cp, "-->", 3)) {
								cp += 3; // skip "-->"
								break;
							}
						}
					} else if('[' == *s1) {
						//skip  "<![endif]-->"
						cp += 12;
						*isExpression = (int *) 0;
					}
					break;
				case 't'://if is textarea then skip over
					if(0 == memcmp("extarea", ++s1, 7)) {
						cp += 10;//<textarea>
						while (*cp) {
							if('<' == *cp && 0 == memcmp("</textarea>", cp, 11)) {
								cp += 11;
								break;
							}
							++cp;
						}
					}
					break;
				case 'T':
					if (0 == memcmp("EXTAREA", ++s1, 7)) {
						cp += 10; //<textarea>
						while (*cp) {
							if('<' == *cp && 0 == memcmp("</TEXTAREA>", cp, 11)) {
								cp += 11;
								break;
							}
							++cp;
						}
					}
					break;
				default:
					if ('<' != *s1) {
						cp += 3; //skip min tag len  "<a>"
					}
					break;
			}
			if (r == 0) {
				return (cp);
			}
		}
		cp++;
	}
	*matchedType = NULL;
	return (NULL);
}

static int htmlParser(request_rec *r, CombinedStyle *combinedStyle, buffer *dstBuf, CombineConfig *pConfig, CombineCtx *ctx) {

	char *maxTagBuf = apr_palloc(r->pool, pConfig->maxUrlLen + 100);
	if(NULL == maxTagBuf) {
		return 0;
	}

	TagConfig *tagConfig = malloc(sizeof(TagConfig));
	if(NULL == tagConfig) {
		return 0;
	}

	LinkedList *groupIndex = malloc(sizeof(LinkedList));
	if(NULL == groupIndex) {
		return 0;
	}
	groupIndex->first = NULL, groupIndex->head = NULL, groupIndex->size = 0;

	apr_hash_t *domains[DOMAIN_COUNTS][2];
	apr_hash_t *duplicates = apr_hash_make(r->pool);

	char *subHtml = ctx->buf->ptr;
	int maxTagSize = (pConfig->maxUrlLen + 98);
	int *matchedType = 0, *isExpression = 0;
	register int isProcessed = 0, i = 0, k = 0, combinBufSize = 100, versionLen = 50;;
	register ParserTag *ptag = NULL;
	register char *curPoint = NULL, *tmpPoint = NULL;
	for(i = 0; i < DOMAIN_COUNTS; i++) {
		domains[i][0] = NULL;
		domains[i][1] = NULL;
	}
	while (NULL != (curPoint = strSearch(subHtml, &matchedType, &isExpression))) {

		tmpPoint = curPoint;

		stringAppend(dstBuf, subHtml, curPoint - subHtml);
		//此时表示当前是js文件，需要使用js的标签来处理
		ptag = (1 == (int) matchedType ? jsPtag:cssPtag);

		//1 skip&filter
		//1 getField 2 getType 3 getPos 4 getAsync
		memcpy(maxTagBuf, ptag->prefix->ptr, ptag->prefix->used);
		for (i = ptag->prefix->used; (curPoint[i] != ptag->suffix) && i < maxTagSize; i++) {
			maxTagBuf[i] = curPoint[i];
		}
		maxTagBuf[i++] = ptag->suffix;
		curPoint += i;
		maxTagBuf[i] = ZERO_END;

		if (1 == ptag->styleType) {
			/**
			 * js 的特殊处理，需要将结束符找出来，</script>
			 * 结束符中间可能有空格或制表符，所以忽略这些
			 * 如果没有结束符，将不进行处理.
			 */
			for(; (isspace(*curPoint) && *curPoint != ZERO_END); curPoint++);

			if (memcmp(ptag->closeTag->ptr, curPoint, ptag->closeTag->used) != 0) {
				//找不到结束的</script>
				stringAppend(dstBuf, maxTagBuf, i);
				subHtml = curPoint;
				continue;
			}
			curPoint += ptag->closeTag->used;
		}

		StyleField *styleField = tagParser(pConfig, ptag, maxTagBuf, i);
		if (NULL == styleField) {
			stringAppend(dstBuf, maxTagBuf, i);
			subHtml = curPoint;
			style_field_free(styleField);
			continue;
		}

		tagConfig->domain = pConfig->newDomains[styleField->domainIndex];
		tagConfig->styleUri = styleField->styleUri;
		tagConfig->async = styleField->async;
		tagConfig->group = styleField->group;
		//process expression <!--[if IE]> for js/css CAN'T clean duplicate
		if(1 == (int) isExpression) {
			//拿uri去获取版本号，url有可以是用户已经拼好的，需要做拆分后再获取版本
			char *singleUri = apr_palloc(r->pool, styleField->styleUri->size);
			buffer *nversion = getStrVersion(styleField->styleUri, singleUri, r, pConfig);
			fillTagConfigParams(tagConfig, nversion, 0, ptag->styleType, 0);
			styleField->version = nversion;

			addExtStyle(dstBuf, tagConfig);
			subHtml = curPoint;
			style_field_free(styleField);
			continue;
		}

		//clean duplicate
		if(isRepeat(r, duplicates, styleField)) {
			subHtml = curPoint;
			style_field_free(styleField);
			continue;
		}

		isProcessed = 1;

		//拿uri去获取版本号，url有可以是用户已经拼好的，需要做拆分后再获取版本
		char *singleUri = apr_palloc(r->pool, styleField->styleUri->size);
		buffer *nversion = getStrVersion(styleField->styleUri, singleUri, r, pConfig);
		styleField->version = nversion;
		fillTagConfigParams(tagConfig, nversion, 0, ptag->styleType, 0);
		//当没有使用异步并且又没有设置位置则保持原位不动
		if(0 == styleField->async && NONE == styleField->postion) {
			addExtStyle(dstBuf, tagConfig);
			subHtml = curPoint;
			style_field_free(styleField);
			continue;
		}
		/**
		 * domains[0][1] //js styleMap
		 * domains[0][2] //css styleMap
		 *
		 * Map<group,LinkedList> styleTypeMap
		 */
		if(styleField->async) {
			//FIXME:add async process
		}
		LinkedList *list = NULL;
		apr_hash_t *styleMap = domains[styleField->domainIndex][ptag->styleType];
		if(NULL == styleMap) {
			styleMap = apr_hash_make(r->pool);
			domains[styleField->domainIndex][ptag->styleType] = styleMap;
		} else {
			list = apr_hash_get(styleMap, styleField->group->ptr, styleField->group->used);
		}
		if(NULL != list) {
			add(list, styleField);
		} else {
			list = malloc(sizeof(LinkedList));
			if(NULL == list) {
				//FIXME:这儿失败了应该让当前的style原位输出
				continue;
			}
			list->first = NULL, list->head = NULL, list->size = 0;
			add(list, styleField);
			/**
			 * 通过使用hash来控制每个group对应一个list
			 */
			apr_hash_set(styleMap, styleField->group->ptr, styleField->group->used, list);
			/**
			 * 将所有group按出现的顺序放入一个list；合并style时按这个顺序输出到页面上。
			 */
			add(groupIndex, list);
		}
		//clean \r\n \n \t & empty char
		while(isspace(*curPoint)) {
			++curPoint;
		}
		subHtml = curPoint;
	}
	if(isProcessed) {
		//append the tail html
		int subHtmlLen = (ctx->buf->ptr + ctx->buf->used) - subHtml;
		stringAppend(dstBuf, subHtml, subHtmlLen);
		if(0 == ctx->debugMode) {
			//create
			CombinedStyle tmpCombine, styleVersion;
			tmpCombine.topBuf = buffer_init_size(combinBufSize);
			tmpCombine.headBuf = buffer_init_size(combinBufSize);
			tmpCombine.footerBuf = buffer_init_size(combinBufSize);

			if(!tmpCombine.topBuf || !tmpCombine.headBuf || !tmpCombine.footerBuf) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "if(isProcessed){has Error}:[%s]", r->unparsed_uri);
				return 0;
			}
			styleVersion.topBuf = buffer_init_size(versionLen);
			styleVersion.headBuf = buffer_init_size(versionLen);
			styleVersion.footerBuf = buffer_init_size(versionLen);
			if(!styleVersion.topBuf || !styleVersion.headBuf || !styleVersion.footerBuf) {
				ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server, "if(isProcessed){has Error}:[%s]", r->unparsed_uri);
				return 0;
			}
			ListNode *node = groupIndex->first;
			while(NULL != node) {
				combineStyles(pConfig, tagConfig, (LinkedList *) node->value, combinedStyle, &tmpCombine, &styleVersion);
				node = (ListNode *) node->next;
				//reset buf
				combinedStyle_clean(&tmpCombine);
				combinedStyle_clean(&styleVersion);
			}
			//free
			combinedStyle_free(&styleVersion);
			combinedStyle_free(&tmpCombine);
		} else if(2 == ctx->debugMode){
			//debug mode 2
			ListNode *node = groupIndex->first;
			while(NULL != node) {
				combineStylesDebug(pConfig, tagConfig, (LinkedList *) node->value, combinedStyle);
				node = (ListNode *)node->next;
			}
		}
	}
	if(0 != dstBuf->size) {
		dstBuf->ptr[dstBuf->used] = ZERO_END;
	}

	for(i = 0; i < DOMAIN_COUNTS; i++) {
		for(k = 0; k < 2; k++) {
			apr_hash_t *styleMap = domains[i][k];
			if(NULL == styleMap) {
				continue;
			}
			apr_hash_index_t *hi;
			for (hi = apr_hash_first(NULL, styleMap); hi; hi = apr_hash_next(hi)) {
				void *value;
				apr_hash_this(hi, NULL, NULL, &value);
				if(NULL == value) {
					continue;
				}
				linkedList_free((LinkedList *) value);
			}
		}
	}
	if(NULL != duplicates) {
		apr_hash_clear(duplicates);
	}
	//free groupIndex, tagConfig
	free(groupIndex); free(tagConfig);
	return isProcessed;
}

static void *configServerCreate(apr_pool_t *p, server_rec *s) {

	CombineConfig *pConfig = apr_palloc(p, sizeof(CombineConfig));
	if(NULL == pConfig) {
		return NULL;
	}
	JS_TAG_EXT_PREFIX_LEN = strlen(JS_TAG_EXT_PREFIX_TXT);
	JS_TAG_EXT_SUFFIX_LEN = strlen(JS_TAG_EXT_SUFFIX_TXT);

	JS_TAG_PREFIX_LEN = strlen(JS_TAG_PREFIX_TXT);
	JS_TAG_SUFFIX_LEN = strlen(JS_TAG_SUFFIX_TXT);

	CSS_PREFIX_LEN = strlen(CSS_PREFIX_TXT);
	CSS_SUFFIX_LEN = strlen(CSS_SUFFIX_TXT);

	pConfig->enabled = 0;
	pConfig->printLog = 0;
	pConfig->filterCntType = NULL;
	pConfig->appName = "modCombine";
	/**
	 * see http://support.microsoft.com/kb/208427/EN-US
	 * default len for ie 2083 char
	 */
	pConfig->maxUrlLen = 2083;

	jsPtag = apr_palloc(p, sizeof(ParserTag));
	if(NULL == jsPtag) {
		return NULL;
	}
	cssPtag = apr_palloc(p, sizeof(ParserTag));
	if(NULL == cssPtag) {
		return NULL;
	}
	// js config
	buffer *jsPrefix = apr_palloc(p, sizeof(buffer));
	if(NULL == jsPrefix) {
		return NULL;
	}
	jsPrefix->ptr = "<script";
	jsPrefix->used = strlen(jsPrefix->ptr);
	jsPrefix->size = jsPrefix->used;

	buffer *jsCloseTag  = apr_palloc(p, sizeof(buffer));
	if(NULL == jsCloseTag) {
		return NULL;
	}
	jsCloseTag->ptr = "</script>";
	jsCloseTag->used = strlen(jsCloseTag->ptr);
	jsCloseTag->size = jsCloseTag->used;

	buffer *jsMark  = apr_palloc(p, sizeof(buffer));
	if(NULL == jsMark) {
		return NULL;
	}
	jsMark->ptr = "src";
	jsMark->used = strlen(jsMark->ptr);
	jsMark->size = jsMark->used;

	buffer *jsRefTag  = apr_palloc(p, sizeof(buffer));
	if(NULL == jsRefTag) {
		return NULL;
	}
	jsRefTag->ptr = " src=";
	jsRefTag->used = strlen(jsRefTag->ptr);
	jsRefTag->size = jsRefTag->used;

	jsPtag->prefix = jsPrefix;
	jsPtag->mark = jsMark;
	jsPtag->refTag = jsRefTag;
	jsPtag->suffix = '>';
	jsPtag->closeTag = jsCloseTag;
	jsPtag->styleType = 1;

	//css config
	buffer *cssPrefix = apr_palloc(p, sizeof(buffer));
	if(NULL == cssPrefix) {
		return NULL;
	}
	cssPrefix->ptr = "<link";
	cssPrefix->used = strlen(cssPrefix->ptr);
	cssPrefix->size = cssPrefix->used;

	buffer *cssRefTag  = apr_palloc(p, sizeof(buffer));
	if(NULL == cssRefTag) {
		return NULL;
	}
	cssRefTag->ptr = " href=";
	cssRefTag->used = strlen(cssRefTag->ptr);
	cssRefTag->size = cssRefTag->used;

	buffer *cssCloseTag  = apr_palloc(p, sizeof(buffer));
	if(NULL == cssCloseTag) {
		return NULL;
	}
	cssCloseTag->ptr = ">";
	cssCloseTag->used = strlen(cssCloseTag->ptr);
	cssCloseTag->size = cssCloseTag->used;

	buffer *cssMark  = apr_palloc(p, sizeof(buffer));
	if(NULL == cssMark) {
		return NULL;
	}
	cssMark->ptr = "stylesheet";
	cssMark->used = strlen(cssMark->ptr);
	cssMark->size = cssMark->used;

	cssPtag->prefix = cssPrefix;
	cssPtag->mark = cssMark;
	cssPtag->refTag = cssRefTag;
	cssPtag->suffix = '>';
	cssPtag->closeTag = cssCloseTag;
	cssPtag->styleType = 0;
	return pConfig;
}

static void styleCombineInsert(request_rec *r) {
	CombineConfig *pConfig = ap_get_module_config(r->server->module_config, &styleCombine_module);
	if(!pConfig->enabled) {
		ap_log_error(APLOG_MARK, APLOG_INFO, 0, r->server, "not support styleCombineModule!");
		return;
	}
	ap_add_output_filter(STYLE_COMBINE_NAME, NULL, r, r->connection);
	return;
}

static apr_status_t styleCombineOutputFilter(ap_filter_t *f, apr_bucket_brigade *pbbIn) {

	request_rec *r      = f->r;
	conn_rec    *c      = r->connection;
	CombineCtx  *ctx    = f->ctx;
	server = r->server;

	if (APR_BRIGADE_EMPTY(pbbIn)) {
		return APR_SUCCESS;
	}
	const char * encode = apr_table_get(r->headers_out, "Content-Encoding");
	if(encode && 0 == strcasecmp(encode, "gzip")) {
		return ap_pass_brigade(f->next, pbbIn);
	}
	if(NULL != apr_table_get(r->notes, STYLE_COMBINE_NAME)) {
		return ap_pass_brigade(f->next, pbbIn);
	}

	CombineConfig *pConfig = NULL;
	pConfig = ap_get_module_config(r->server->module_config, &styleCombine_module);

	if(NULL == pConfig) {
		return ap_pass_brigade(f->next, pbbIn);
	}
	if(r->content_type && r->content_type[0]) {
		char *contentType = apr_pstrdup(r->pool, r->content_type);
		char *pt = contentType;
		for(; pt && *pt; ++pt) {
			if ((';' != *pt) && (' ' != *pt)) {
				*pt = tolower(*pt);
				continue;
			}
			*pt = ';';
			*(++pt) = ZERO_END;
			break;
		}
		if(!strstr(pConfig->filterCntType, contentType)) {
			return ap_pass_brigade(f->next, pbbIn);
		}
	}

	/**
	 * 1add runMode
	 * 添加模块的动态开关，由版本文件内容来控制
	 */
	if(NULL != appRunMode && 0 == memcmp(appRunMode, RUN_MODE_STATUS, 3)) {
		loadStyleVersion(r, pConfig);
		return ap_pass_brigade(f->next, pbbIn);
	}
	/**
	 * 2 block & white list
	 */
	/**
	 * 3add debugMode
	 * 本次请求禁用此模块，用于开发调试使用
	 */
	int debugMode = 0;
	if(NULL != r->parsed_uri.query) {
		char *debugModeParam = strstr(r->parsed_uri.query, DEBUG_MODE);
		if(NULL != debugModeParam) {
			debugModeParam += DEBUG_MODE_LEN;
			if(*debugModeParam == '1') {
				debugMode = 1;
				return ap_pass_brigade(f->next, pbbIn);
			}
			if(*debugModeParam == '2') {
				debugMode = 2;
			}
		}
	}
	if (NULL == ctx) {
		ctx = f->ctx = apr_palloc(r->pool, sizeof(*ctx));
		if(NULL == ctx) {
			return ap_pass_brigade(f->next, pbbIn);
		}
		ctx->pbbOut = apr_brigade_create(r->pool, c->bucket_alloc);
		if(NULL == ctx->pbbOut) {
			return ap_pass_brigade(f->next, pbbIn);
		}
		ctx->buf = buffer_init_size(DEFAULT_CONTENT_LEN);
		if(NULL == ctx->buf) {
			return ap_pass_brigade(f->next, pbbIn);
		}
		//set debugMode value
		ctx->debugMode = debugMode;
	}
	int isEOS = 0;
	apr_bucket *pbktIn = NULL;
	for (pbktIn = APR_BRIGADE_FIRST(pbbIn);
	            pbktIn != APR_BRIGADE_SENTINEL(pbbIn);
	            pbktIn = APR_BUCKET_NEXT(pbktIn)) {

		//the end
		if(APR_BUCKET_IS_EOS(pbktIn)) {
			isEOS = 1;
			break;
		}
		const char *data;
		apr_size_t len;
		//read len
		apr_bucket_read(pbktIn, &data, &len, APR_BLOCK_READ);
		stringAppend(ctx->buf, data, len);

		apr_bucket_delete(pbktIn);
	}
	if(!isEOS) {
		return OK;
	}
	struct timeval start;
	if(9 == pConfig->printLog) {
		gettimeofday(&start, NULL);
	}
	if(ctx->buf->used > 0) {
		ctx->buf->ptr[ctx->buf->used] = ZERO_END;

		CombinedStyle combinedStyle;
		combinedStyle.footerBuf = buffer_init();
		combinedStyle.topBuf = buffer_init();
		combinedStyle.headBuf = buffer_init();
		//load version
		loadStyleVersion(r, pConfig);

		buffer *dstBuf = buffer_init_size(ctx->buf->used);
		if(dstBuf && combinedStyle.footerBuf && combinedStyle.topBuf && combinedStyle.headBuf) {
			//if find any style
			if(htmlParser(r, &combinedStyle, dstBuf, pConfig, ctx)) {
				resetHtml(c, ctx->pbbOut, &combinedStyle, dstBuf);
			} else {
				addBucket(c, ctx->pbbOut, ctx->buf->ptr, ctx->buf->used);
			}
		} else {
			addBucket(c, ctx->pbbOut, ctx->buf->ptr, ctx->buf->used);
		}
		//free
		combinedStyle_free(&combinedStyle);
		buffer_free(dstBuf);
	}
	//append eos
	APR_BRIGADE_INSERT_TAIL(ctx->pbbOut, apr_bucket_eos_create(c->bucket_alloc));
	apr_table_get(r->notes, "ok");
	buffer_free(ctx->buf);
	apr_brigade_cleanup(pbbIn);

	if(9 == pConfig->printLog) {
		struct timeval end;
		gettimeofday(&end, NULL);
		ap_log_error(APLOG_MARK, APLOG_ERR, 0, r->server,
				"===end process: URI[%s]==start[%ld]==end[%ld]==Result[%ld]",
				r->uri, start.tv_usec, end.tv_usec, end.tv_usec - start.tv_usec);
	}
	return ap_pass_brigade(f->next, ctx->pbbOut);
}

static const char *setEnabled(cmd_parms *cmd, void *dummy, int arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	pConfig->enabled = arg;
	return NULL;
}

static const char *setFilterCntType(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if ((NULL == arg) || (strlen(arg) <= 1)) {
		return "styleCombine filterCntType value may not be null.";
	} else {
		pConfig->filterCntType = apr_pstrdup(cmd->pool, arg);
	}
	return NULL;
}

static const char *setAppName(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if ((NULL == arg) || (strlen(arg) <= 1)) {
		return "styleCombine appName can't be null OR empty";
	} else {
		pConfig->appName = apr_pstrdup(cmd->pool, arg);
	}
	return NULL;
}

//parse domain
static void parseDomain(cmd_parms *cmd, buffer *domains[], const char *domainsArg) {
	if(NULL == domainsArg || NULL == domains) {
		return;
	}
	int i = 0;
	char *tmpDomain = domainsArg;
	for(i = 0; i < DOMAIN_COUNTS; i++) {
		char *domain = strchr(tmpDomain, ';');
		if(NULL != domain) {
			buffer *buf = apr_palloc(cmd->pool, sizeof(buffer));
			buf->ptr = NULL, buf->used = 0, buf->size = 0;
			stringAppend(buf, tmpDomain, (domain - tmpDomain));
			domains[i] = buf;
			tmpDomain = ++domain;//move char of ';'
		}
	}
}

static const char *setOldDomains(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if ((NULL == arg) || (strlen(arg) <= 1)) {
		return "styleCombine old domain value may not be null";
	} else {
		parseDomain(cmd, pConfig->oldDomains, arg);
	}
	return NULL;
}

static const char *setNewDomains(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if ((NULL == arg) || (strlen(arg) <= 1)) {
		return "styleCombine new domain value may not be null";
	} else {
		parseDomain(cmd, pConfig->newDomains, arg);
	}
	return NULL;
}

static const char *setMaxUrlLen(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	int len = 0;
	if ((NULL == arg) || (len = atoi(arg)) < pConfig->maxUrlLen) {
		ap_log_error(APLOG_MARK, LOG_ERR, 0, cmd->server, "maxUrlLen too small, will set default  2083!");
	} else {
		pConfig->maxUrlLen = len;
	}
	return NULL;
}

static const char *setPrintLog(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if(NULL != arg) {
		pConfig->printLog = atoi(arg);
	}
	return NULL;
}

static const char *setVersionFilePath(cmd_parms *cmd, void *dummy, const char *arg) {
	CombineConfig *pConfig = ap_get_module_config(cmd->server->module_config, &styleCombine_module);
	if(NULL != arg) {
		pConfig->versionFilePath = apr_pstrdup(cmd->pool, arg);
	} else {
		return "styleCombine versionFilePath value may not be null";
	}
	return NULL;
}

static const command_rec styleCombineCmds[] =
{
		AP_INIT_FLAG("enabled", setEnabled, NULL, OR_ALL, "open or close this module"),

		AP_INIT_TAKE1("appName", setAppName, NULL, OR_ALL, "app name"),

		AP_INIT_TAKE1("filterCntType", setFilterCntType, NULL, OR_ALL, "filter content type"),

		AP_INIT_TAKE1("oldDomains", setOldDomains, NULL, OR_ALL, "style old domain url"),

		AP_INIT_TAKE1("newDomains", setNewDomains, NULL, OR_ALL, "style new domain url"),

		AP_INIT_TAKE1("maxUrlLen", setMaxUrlLen, NULL, OR_ALL, "url max len"),

		AP_INIT_TAKE1("printLog", setPrintLog, NULL, OR_ALL, " set printLog level"),

		// part version command
		AP_INIT_TAKE1("versionFilePath", setVersionFilePath, NULL, OR_ALL, "style versionFilePath dir"),

		{ NULL }
};

static apr_status_t styleCombine_init_module(apr_pool_t *p, apr_pool_t *plog,
											apr_pool_t *tp, server_rec *s) {
	ap_add_version_component(p, MODULE_BRAND);
	return APR_SUCCESS;
}

static void styleCombine_register_hooks(apr_pool_t *p) {
	ap_hook_post_config(styleCombine_init_module, NULL, NULL, APR_HOOK_MIDDLE);
	ap_hook_insert_filter(styleCombineInsert, NULL, NULL, APR_HOOK_MIDDLE);
    ap_register_output_filter(STYLE_COMBINE_NAME, styleCombineOutputFilter, NULL, AP_FTYPE_RESOURCE);
    return;
}

/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA styleCombine_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                  /* create per-dir    config structures */
    NULL,                  /* merge  per-dir    config structures */
    configServerCreate,    /* create per-server config structures */
    NULL,                  /* merge  per-server config structures */
    styleCombineCmds,      /* table of config file commands       */
    styleCombine_register_hooks  /* register hooks                      */
};
