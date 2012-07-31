/*
    webs.c -- GoAhead Embedded HTTP webs server

    MOB - rename
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/******************************** Description *********************************/

/*
 *  This module implements an embedded HTTP/1.1 web server. It supports
 *  loadable URL handlers that define the nature of URL processing performed.
 */

/********************************* Includes ***********************************/

#include    "wsIntrn.h"
#if BIT_DIGEST_AUTH
    #include    "websda.h"
#endif

//  MOB - mov eto header
extern socket_t                **socketList;                   /* List of open sockets */

/******************************** Global Data *********************************/

websStatsType   websStats;              /* Web access stats */
webs_t          *webs;                  /* Open connection list head */
sym_fd_t        websMime;               /* Set of mime types */
int             websMax;                /* List size */
int             websPort;               /* Listen port for server */
char_t          websHost[64];           /* Host name for the server */
char_t          websIpaddr[64];         /* IP address for the server */
char_t          *websHostUrl = NULL;    /* URL to access server */
char_t          *websIpaddrUrl = NULL;  /* URL to access server */

/*********************************** Locals ***********************************/
/*
 *  Standard HTTP error codes
 */

websErrorType websErrors[] = {
    { 200, T("Data follows") },
    { 204, T("No Content") },
    { 301, T("Redirect") },
    { 302, T("Redirect") },
    { 304, T("Use local copy") },
    { 400, T("Page not found") },
    { 401, T("Unauthorized") },
    { 403, T("Forbidden") },
    { 404, T("Site or Page Not Found") },
    { 405, T("Access Denied") },
    { 500, T("Web Error") },
    { 501, T("Not Implemented") },
    { 503, T("Site Temporarily Unavailable. Try again.") },
    { 0, NULL }
};

#if BIT_ACCESS_LOG
static char_t   websLogname[64] = T("log.txt"); /* Log filename */
static int      websLogFd;                      /* Log file handle */
#endif

#if BIT_DEBUG_TRACE
static char_t   websTracename[64] = T("trace.txt"); /* Log filename */
static int      websTraceFd;                        /* Log file handle */
#endif

static int      websListenSock;                 /* Listen socket */
static char_t   websRealm[64] = T("GoAhead");   /* Realm name */

static int      websOpenCount = 0;      /* count of apps using this module */

/**************************** Forward Declarations ****************************/


static int      websGetInput(webs_t wp, char_t **ptext, int *nbytes);
static int      websParseFirst(webs_t wp, char_t *text);
static void     websParseRequest(webs_t wp);
static void     websSocketEvent(int sid, int mask, void* data);
static int      websGetTimeSinceMark(webs_t wp);

#if BIT_ACCESS_LOG
static void     websLog(webs_t wp, int code);
#endif
#if BIT_DEBUG_TRACE
static void     traceHandler(int level, char_t *buf);
#endif
#if BIT_IF_MODIFIED
static time_t   dateParse(time_t tip, char_t *cmd);
#endif

/*********************************** Code *************************************/

int websOpenServer(int port, int retries)
{
    websMimeType    *mt;

    if (++websOpenCount != 1) {
        return websPort;
    }
    a_assert(port > 0);
    a_assert(retries >= 0);

    websDefaultOpen();

#if BIT_ROM
    websRomOpen();
#endif

    webs = NULL;
    websMax = 0;
    /*
        Create a mime type lookup table for quickly determining the content type
     */
    websMime = symOpen(WEBS_SYM_INIT * 4);
    a_assert(websMime >= 0);
    for (mt = websMimeList; mt->type; mt++) {
        symEnter(websMime, mt->ext, valueString(mt->type, 0), 0);
    }

    /*
        Open the URL handler module. The caller should create the required URL handlers after calling this function.
     */
    if (websUrlHandlerOpen() < 0) {
        return -1;
    }
    websFormOpen();

#if BIT_ACCESS_LOG
    //  MOB - refactor
#if !VXWORKS
    websLogFd = gopen(websLogname, O_CREAT | O_TRUNC | O_APPEND | O_WRONLY, 0666);
#else
    websLogFd = gopen(websLogname, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    lseek(fd, 0, SEEK_END);
#endif
    a_assert(websLogFd >= 0);
#endif
    
#if BIT_DEBUG_TRACE
    //  MOB - refactor
#if !VXWORKS
    websTraceFd = gopen(websTracename, O_CREAT | O_TRUNC | O_APPEND | O_WRONLY, 0666);
#else
    websTraceFd = gopen(websTracename, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    lseek(fd, 0, SEEK_END);
#endif
    a_assert(websTraceFd >= 0);
    traceSetHandler(traceHandler);
#endif  
    return websOpenListen(port, retries);
}


void websCloseServer()
{
    webs_t  wp;
    int     wid;

    if (--websOpenCount > 0) {
        return;
    }
    websCloseListen();

    for (wid = websMax; webs && wid >= 0; wid--) {
        if ((wp = webs[wid]) == NULL) {
            continue;
        }
        socketCloseConnection(wp->sid);
        websFree(wp);
    }

#if BIT_ACCESS_LOG
    if (websLogFd >= 0) {
        close(websLogFd);
        websLogFd = -1;
    }
#endif
#if BIT_DEBUG_TRACE
    if (websTraceFd >= 0) {
        close(websTraceFd);
        websTraceFd = -1;
    }
#endif

#if BIT_ROM
    websRomClose();
#endif
    websDefaultClose();
    symClose(websMime);
    websFormClose();
    websUrlHandlerClose();
}


int websOpenListen(int port, int retries)
{
    int     i, orig;

    a_assert(port > 0);
    a_assert(retries >= 0);

    orig = port;

    /*
        Open the webs webs listen port. If we fail, try the next port.
        MOB - not a good idea
     */
    for (i = 0; i <= retries; i++) {
        websListenSock = socketOpenConnection(NULL, port, websAccept, 0);
        if (websListenSock >= 0) {
            break;
        }
        port++;
    }
    if (i > retries) {
        error(E_L, E_USER, T("Couldn't open a socket on ports %d - %d"), orig, port - 1);
        return -1;
    } 

    /*
        Determine the full URL address to access the home page for this web server
     */
    websPort = port;
    bfreeSafe(websHostUrl);
    bfreeSafe(websIpaddrUrl);
    websIpaddrUrl = websHostUrl = NULL;

    if (port == 80) {
        websHostUrl = bstrdup(websHost);
        websIpaddrUrl = bstrdup(websIpaddr);
    } else {
        fmtAlloc(&websHostUrl, WEBS_MAX_URL + 80, T("%s:%d"), websHost, port);
        fmtAlloc(&websIpaddrUrl, WEBS_MAX_URL + 80, T("%s:%d"), websIpaddr, port);
    }
    trace(0, T("webs: Listening for HTTP requests at address %s\n"), websIpaddrUrl);
    return port;
}


void websCloseListen()
{
    if (websListenSock >= 0) {
        socketCloseConnection(websListenSock);
        websListenSock = -1;
    }
    bfreeSafe(websHostUrl);
    bfreeSafe(websIpaddrUrl);
    websIpaddrUrl = websHostUrl = NULL;
}


int websAccept(int sid, char *ipaddr, int port, int listenSid)
{
    webs_t  wp;
    int     wid;
    struct sockaddr_in      ifAddr;
    int len;
    char *pString;

    a_assert(ipaddr && *ipaddr);
    a_assert(sid >= 0);
    a_assert(port >= 0);

    /*
        Allocate a new handle for this accepted connection. This will allocate a webs_t structure in the webs[] list
     */
    if ((wid = websAlloc(sid)) < 0) {
        return -1;
    }
    wp = webs[wid];
    a_assert(wp);
    wp->listenSid = listenSid;

    ascToUni(wp->ipaddr, ipaddr, min(sizeof(wp->ipaddr), strlen(ipaddr) + 1));

    /*
        Get the ip address of the interface that acept the connection.
     */
    len = sizeof(struct sockaddr_in);
    if (getsockname(socketList[sid]->sock, (struct sockaddr *)&ifAddr, (socklen_t *) &len) < 0) {
        return -1;
    }
    pString = inet_ntoa(ifAddr.sin_addr);
    gstrncpy(wp->ifaddr, pString, gstrlen(pString));

    /*
     Check if this is a request from a browser on this system. This is useful to know for permitting administrative
     operations only for local access 
     */
    if (gstrcmp(wp->ipaddr, T("127.0.0.1")) == 0 || 
            gstrcmp(wp->ipaddr, websIpaddr) == 0 || 
            gstrcmp(wp->ipaddr, websHost) == 0) {
        wp->flags |= WEBS_LOCAL_REQUEST;
    }

    /*
        Arrange for websSocketEvent to be called when read data is available
     */
    socketCreateHandler(sid, SOCKET_READABLE, websSocketEvent, wp);

    /*
        Arrange for a timeout to kill hung requests
     */
    wp->timeout = emfSchedCallback(WEBS_TIMEOUT, websTimeout, (void *) wp);
    trace(8, T("webs: accept request\n"));
    return 0;
}


/*
    The webs socket handler.  Called in response to I/O. We just pass control to the relevant read or write handler. A
    pointer to the webs structure is passed as a (void*) in iwp.  
 */
static void websSocketEvent(int sid, int mask, void* iwp)
{
    webs_t  wp;

    wp = (webs_t) iwp;
    a_assert(wp);

    if (! websValid(wp)) {
        return;
    }
    if (mask & SOCKET_READABLE) {
        websReadEvent(wp);
    } 
    if (mask & SOCKET_WRITABLE) {
        if (websValid(wp) && wp->writeSocket) {
            (*wp->writeSocket)(wp);
        }
    } 
}


/*
    The webs read handler. This is the primary read event loop. It uses a state machine to track progress while parsing
    the HTTP request.  Note: we never block as the socket is always in non-blocking mode.
 */
void websReadEvent(webs_t wp)
{
    char_t  *text;
    int     rc, nbytes, len, done, fd, size;

    a_assert(wp);
    a_assert(websValid(wp));

    websSetTimeMark(wp);

    /*
        Read as many lines as possible. socketGets is called to read the header and socketRead is called to read posted
        data.  
     */
    text = NULL;
    fd = -1;
    for (done = 0; !done; ) {
        if (text) {
            bfree(text);
            text = NULL;
        }

        /*
             Get more input into "text". Returns 0, if more data is needed to continue, -1 if finished with the request,
             or 1 if all required data is available for current state.
         */
        while ((rc = websGetInput(wp, &text, &nbytes)) == 0) {
            ;
        }

        /*
            websGetInput returns -1 if it finishes with the request
         */
        if (rc < 0) {
            break;
        }

        /*
            This is the state machine for the web server. 
         */
        switch(wp->state) {
        case WEBS_BEGIN:
            /*
                Parse the first line of the Http header
             */
            if (websParseFirst(wp, text) < 0) {
                done++;
                break;
            }
            wp->state = WEBS_HEADER;
            break;
        
        case WEBS_HEADER:
            /*
                Store more of the HTTP header. As we are doing line reads, we need to separate the lines with '\n'
             */
            if (ringqLen(&wp->header) > 0) {
                ringqPutStr(&wp->header, T("\n"));
            }
            ringqPutStr(&wp->header, text);
            break;

        case WEBS_POST_CLEN:
            /*
                POST request with content specified by a content length.  If this is a CGI request, write the data to
                the cgi stdin.
             */
#ifndef __NO_CGI_BIN
            if (wp->flags & WEBS_CGI_REQUEST) {
                if (fd == -1) {
#if !defined(WIN32)
                    fd = gopen(wp->cgiStdin, O_CREAT | O_WRONLY | O_BINARY, 0666);
#else
                    _sopen_s(&fd, wp->cgiStdin, O_CREAT | O_WRONLY | O_BINARY, _SH_DENYNO, 0666);
#endif
                }
                gwrite(fd, text, nbytes);
            } else 
#endif
            if (wp->query) {
                if (wp->query[0] && !(wp->flags & WEBS_POST_DATA)) {
                    /*
                        Special case where the POST request also had query data specified in the URL, ie.
                        url?query_data. In this case the URL query data is separated by a '&' from the posted query
                        data.
                     */
                    len = gstrlen(wp->query);
                    if (text) {
                        size = (len + gstrlen(text) + 2) * sizeof(char_t);
                        wp->query = brealloc(wp->query, size);
                        wp->query[len++] = '&';
#if !defined(WIN32)
                        strcpy(&wp->query[len], text);
#else
                        strcpy_s(&wp->query[len], size - len, text);
#endif
                    }

                } else {
                    /*
                        The existing query data came from the POST request so just append it.
                     */
                    if (text != NULL) {
                        len = gstrlen(wp->query);
                        size = (len +   gstrlen(text) + 1) * sizeof(char_t);
                        wp->query = brealloc(wp->query, size);
                        if (wp->query) {
#if !defined(WIN32)
                            gstrcpy(&wp->query[len], text);
#else
                            strcpy_s(&wp->query[len], size - len, text);
#endif
                        }
                    }
                }

            } else {
                wp->query = bstrdup(text);
            }
            /*
                Calculate how much more post data is to be read.
             */
            wp->flags |= WEBS_POST_DATA;
            wp->clen -= nbytes;
            if (wp->clen > 0) {
                if (nbytes > 0) {
                    break;
                }
                done++;
                break;
            }
            /*
                No more data so process the request, (but be sure to close the input file first!).
             */
            if (fd != -1) {
                gclose (fd);
                fd = -1;
            }
            websUrlHandlerRequest(wp);
            done++;
            break;

        case WEBS_POST:
            /*
                POST without content-length specification. If this is a CGI request, write the data to the cgi stdin.
                socketGets was used to get the data and it strips \n's so add them back in here.
             */
#ifndef __NO_CGI_BIN
            if (wp->flags & WEBS_CGI_REQUEST) {
                if (fd == -1) {
#if !defined(WIN32)
                    fd = gopen(wp->cgiStdin, O_CREAT | O_WRONLY | O_BINARY, 0666);
#else
                    _sopen_s(&fd, wp->cgiStdin, O_CREAT | O_WRONLY | O_BINARY, _SH_DENYNO, 0666);
#endif
                }
                gwrite(fd, text, nbytes);
                gwrite(fd, T("\n"), sizeof(char_t));
            } else
#endif
            if (wp->query && *wp->query && !(wp->flags & WEBS_POST_DATA)) {
                len = gstrlen(wp->query);
                size = (len + gstrlen(text) + 2) * sizeof(char_t);
                wp->query = brealloc(wp->query, size);
                if (wp->query) {
                    wp->query[len++] = '&';
#if !defined(WIN32)
                    gstrcpy(&wp->query[len], text);
#else
                    strcpy_s(&wp->query[len], size - len, text);
#endif
                }

            } else {
                wp->query = bstrdup(text);
            }
            wp->flags |= WEBS_POST_DATA;
            done++;
            break;

        default:
            websError(wp, 404, T("Bad state"));
            done++;
            break;
        }
    }
    if (fd != -1) {
        fd = gclose (fd);
    }
    if (text) {
        bfree(text);
    }
}


/*
    Get input from the browser. Return TRUE (!0) if the request has been handled. Return -1 on errors or if the request
    has been processed, 1 if input read, and 0 to instruct the caller to call again for more input.
  
    Note: socketRead will Return the number of bytes read if successful. This may be less than the requested "bufsize"
    and may be zero. It returns -1 for errors. It returns 0 for EOF. Otherwise it returns the number of bytes read.
    Since this may be zero, callers should use socketEof() to distinguish between this and EOF.
 */
static int websGetInput(webs_t wp, char_t **ptext, int *pnbytes) 
{
    char_t  *text;
    char    buf[WEBS_SOCKET_BUFSIZ+1];
    int     nbytes, len, clen;

    a_assert(websValid(wp));
    a_assert(ptext);
    a_assert(pnbytes);

    *ptext = text = NULL;
    *pnbytes = 0;

    /*
        If this request is a POST with a content length, we know the number
        of bytes to read so we use socketRead().
     */
    if (wp->state == WEBS_POST_CLEN) {
        len = (wp->clen > WEBS_SOCKET_BUFSIZ) ? WEBS_SOCKET_BUFSIZ : wp->clen;
    } else {
        len = 0;
    }
    if (len > 0) {

#if BIT_PACK_SSL
        if (wp->flags & WEBS_SECURE) {
            nbytes = websSSLRead(wp->wsp, buf, len);
        } else {
            nbytes = socketRead(wp->sid, buf, len);
        }
#else
        nbytes = socketRead(wp->sid, buf, len);
#endif
        if (nbytes < 0) {                       /* Error */
            websDone(wp, 0);
            return -1;

        }  else if (nbytes == 0) {              /* EOF or No data available */
            /*
               Infinite CPU usage if not all post data is sent. This is a side-effect of socketRead whose return value
               does not distinguish between EOF and no-data and we have to explicitly use the socketEof() to test for it.
             */
            if (socketEof(wp->sid)) {
                websDone(wp, 0);
            }
            return -1;

        } else {                                /* Valid data */
            /*
                Convert to UNICODE if necessary.  First be sure the string is NULL terminated.
             */
            buf[nbytes] = '\0';
            if ((text = ballocAscToUni(buf, nbytes)) == NULL) {
                websError(wp, 503, T("Insufficient memory"));
                return -1;
            }
        }

    } else {
#if BIT_PACK_SSL
        if (wp->flags & WEBS_SECURE) {
            nbytes = websSSLGets(wp->wsp, &text);
        } else {
            nbytes = socketGets(wp->sid, &text);
        }
#else
        nbytes = socketGets(wp->sid, &text);
#endif

        if (nbytes < 0) {
            int eof;
            /*
                Error, EOF or incomplete
             */
#if BIT_PACK_SSL
            if (wp->flags & WEBS_SECURE) {
                /*
                    If state is WEBS_BEGIN and the request is secure, a -1 will usually indicate SSL negotiation
                 */
                if (wp->state == WEBS_BEGIN) {
                    eof = 1;
                } else {
                    eof = websSSLEof(wp->wsp);
                }
            } else {
                eof = socketEof(wp->sid);
            }
#else
            eof = socketEof(wp->sid);
#endif

            if (eof) {
                /*
                    If this is a post request without content length, process the request as we now have all the data.
                    Otherwise just close the connection.
                 */
                if (wp->state == WEBS_POST) {
                    websUrlHandlerRequest(wp);
                } else {
                    websDone(wp, 0);
                }
            } else {
                /*
                    If an error occurred and it wasn't an eof, close the connection
                 */
                websDone(wp, 0);
            }
            /*
                If state is WEBS_HEADER and the ringq is empty, then this is a simple request with no additional header
                fields to process and no empty line terminator. NOTE: this fix for earlier versions of browsers is
                troublesome because if we don't receive the entire header in the first pass this code assumes we were
                only expecting a one line header, which is not necessarily the case. So we weren't processing the whole
                header and weren't fufilling requests properly. 
            */
            return -1;

        } else if (nbytes == 0) {
            if (wp->state == WEBS_HEADER) {
                /*
                    Valid empty line, now finished with header
                 */
                websParseRequest(wp);
                if (wp->flags & WEBS_POST_REQUEST) {
                    if (wp->flags & WEBS_CLEN) {
                        wp->state = WEBS_POST_CLEN;
                        clen = wp->clen;
                    } else {
                        wp->state = WEBS_POST;
                        clen = 1;
                    }
                    if (clen > 0) {
                        /*
                            Return 0 to get more data.
                         */
                        return 0;
                    }
                    return 1;
                }
                /*
                    We've read the header so go and handle the request
                 */
                websUrlHandlerRequest(wp);
            }
            return -1;
        }
    }
    a_assert(text);
    a_assert(nbytes > 0);
    *ptext = text;
    *pnbytes = nbytes;
    return 1;
}


/*
    Parse the first line of a HTTP request
 */
static int websParseFirst(webs_t wp, char_t *text)
{
    char_t  *op, *proto, *protoVer, *url, *host, *query, *path, *port, *ext;
    char_t  *buf;
    int     testPort;

    a_assert(websValid(wp));
    a_assert(text && *text);

    /*
        Determine the request type: GET, HEAD or POST
     */
    op = gstrtok(text, T(" \t"));
    if (op == NULL || *op == '\0') {
        websError(wp, 400, T("Bad HTTP request"));
        return -1;
    }
    if (gstrcmp(op, T("GET")) != 0) {
        if (gstrcmp(op, T("POST")) == 0) {
            wp->flags |= WEBS_POST_REQUEST;
        } else if (gstrcmp(op, T("HEAD")) == 0) {
            wp->flags |= WEBS_HEAD_REQUEST;
        } else {
            websError(wp, 400, T("Bad request type"));
            return -1;
        }
    }

    /*
        Store result in the form (CGI) variable store
     */
    websSetVar(wp, T("REQUEST_METHOD"), op);

    url = gstrtok(NULL, T(" \t\n"));
    if (url == NULL || *url == '\0') {
        websError(wp, 400, T("Bad HTTP request"));
        return -1;
    }
    protoVer = gstrtok(NULL, T(" \t\n"));

    /*
        Parse the URL and store all the various URL components. websUrlParse returns an allocated buffer in buf which we
        must free. We support both proxied and non-proxied requests. Proxied requests will have http://host/ at the
        start of the URL. Non-proxied will just be local path names.
     */
    host = path = port = proto = query = ext = NULL;
    if (websUrlParse(url, &buf, &host, &path, &port, &query, &proto, NULL, &ext) < 0) {
        websError(wp, 400, T("Bad URL format"));
        return -1;
    }
    wp->url = bstrdup(url);

#ifndef __NO_CGI_BIN
    if (gstrstr(url, CGI_BIN) != NULL) {
        wp->flags |= WEBS_CGI_REQUEST;
        if (wp->flags & WEBS_POST_REQUEST) {
            wp->cgiStdin = websGetCgiCommName();
        }
    }
#endif
    wp->query = bstrdup(query);
    wp->host = bstrdup(host);
    wp->path = bstrdup(path);
    wp->protocol = bstrdup(proto);
    wp->protoVersion = bstrdup(protoVer);
    
    if ((testPort = socketGetPort(wp->listenSid)) >= 0) {
        wp->port = testPort;
    } else {
        wp->port = gatoi(port);
    }
    if (gstrcmp(ext, T(".asp")) == 0) {
        wp->flags |= WEBS_ASP;
    }
    bfree(buf);
    websUrlType(url, wp->type, TSZ(wp->type));
    ringqFlush(&wp->header);
    return 0;
}


/*
    Parse a full request
 */
#define isgoodchar(s) (gisalnum((s)) || ((s) == '/') || ((s) == '_') || ((s) == '.')  || ((s) == '-') )

static void websParseRequest(webs_t wp)
{
    char_t  *authType, *upperKey, *cp, *browser, *lp, *key, *value;

    a_assert(websValid(wp));

    /*
        Define default CGI values
     */
    websSetVar(wp, T("HTTP_AUTHORIZATION"), T(""));

    /* 
        Parse the header and create the Http header keyword variables
        We rewrite the header as we go for non-local requests.  NOTE: this
        modifies the header string directly and tokenizes each line with '\0'.
    */
    browser = NULL;
    for (lp = (char_t*) wp->header.servp; lp && *lp; ) {
        cp = lp;
        if ((lp = gstrchr(lp, '\n')) != NULL) {
            lp++;
        }
        if ((key = gstrtok(cp, T(": \t\n"))) == NULL) {
            continue;
        }
        if ((value = gstrtok(NULL, T("\n"))) == NULL) {
            value = T("");
        }
        while (gisspace(*value)) {
            value++;
        }
        strlower(key);

        /*
            Create a variable (CGI) for each line in the header
         */
        fmtAlloc(&upperKey, (gstrlen(key) + 6), T("HTTP_%s"), key);
        for (cp = upperKey; *cp; cp++) {
            if (*cp == '-') {
                *cp = '_';
            }
        }
        strupper(upperKey);
        websSetVar(wp, upperKey, value);
        bfree(upperKey);

        /*
            Track the requesting agent (browser) type
         */
        if (gstrcmp(key, T("user-agent")) == 0) {
            wp->userAgent = bstrdup(value);

        /*
            Parse the user authorization. ie. password
         */
        } else if (gstricmp(key, T("authorization")) == 0) {
            /*
                Determine the type of Authorization Request
             */
            authType = bstrdup (value);
            a_assert (authType);
            /*          
                Truncate authType at the next non-alpha character
             */
            cp = authType;
            while (gisalpha(*cp)) {
                cp++;
            }
            *cp = '\0';

            wp->authType = bstrdup(authType);
            bfree(authType);

            if (gstricmp(wp->authType, T("basic")) == 0) {
                char_t  userAuth[FNAMESIZE];
                /*
                    The incoming value is username:password (Basic authentication)
                 */
                if ((cp = gstrchr(value, ' ')) != NULL) {
                    *cp = '\0';
                   bfree(wp->authType);
                    wp->authType = bstrdup(value);
                    websDecode64(userAuth, ++cp, sizeof(userAuth));
                } else {
                    websDecode64(userAuth, value, sizeof(userAuth));
                }
                /*
                    Split userAuth into userid and password
                 */
                if ((cp = gstrchr(userAuth, ':')) != NULL) {
                    *cp++ = '\0';
                }
                if (cp) {
                    wp->userName = bstrdup(userAuth);
                    wp->password = bstrdup(cp);
                } else {
                    wp->userName = bstrdup(T(""));
                    wp->password = bstrdup(T(""));
                }
                /*
                    Set the flags to indicate digest authentication
                 */
                wp->flags |= WEBS_AUTH_BASIC;
            } else {
#if BIT_DIGEST_AUTH
                /*
                    The incoming value is slightly more complicated (Digest)
                 */
                char_t *np;     /* pointer to end of tag name */
                char_t tp;      /* temporary character holding space */
                char_t *vp;     /* pointer to value */
                char_t *npv;    /* pointer to end of value, "next" pointer */
                char_t tpv;     /* temporary character holding space */

                /*
                    Set the flags to indicate digest authentication
                 */
                wp->flags |= WEBS_AUTH_DIGEST;
                /*
                    Move cp to Next word beyond "Digest", vp to first char after '='.
                 */
                cp = value;
                while (isgoodchar(*cp)) {
                    cp++;
                }
                while (!isgoodchar(*cp)) {
                    cp++;
                }

                /*
                    Find beginning of value
                 */
                vp = gstrchr(cp, '=');
                while (vp) {
                    /*
                        Zero-terminate tag name
                     */
                    np = cp;
                    while (isgoodchar(*np)) {
                        np++;
                    }
                    tp = *np;
                    *np = 0;
                    /*
                        Advance value pointer to first legit character
                     */
                    vp++;
                    while (!isgoodchar(*vp)) {
                        vp++;
                    }
                    /*
                        Zero-terminate value
                     */
                    npv = vp;
                    while (isgoodchar(*npv)) {
                        npv++;
                    }
                    tpv = *npv;
                    *npv = 0;
                    /*
                        Extract the fields
                     */
                    if (gstricmp(cp, T("username")) == 0) {
                        wp->userName = bstrdup(vp);
                    } else if (gstricmp(cp, T("response")) == 0) {
                        wp->digest = bstrdup(vp);
                    } else if (gstricmp(cp, T("opaque")) == 0) {
                        wp->opaque = bstrdup(vp);
                    } else if (gstricmp(cp, T("uri")) == 0) {
                        wp->uri = bstrdup(vp);
                    } else if (gstricmp(cp, T("realm")) == 0) {
                        wp->realm = bstrdup(vp);
                    } else if (gstricmp(cp, T("nonce")) == 0) {
                        wp->nonce = bstrdup(vp);
                    } else if (gstricmp(cp, T("nc")) == 0) {
                        wp->nc = bstrdup(vp);
                    } else if (gstricmp(cp, T("cnonce")) == 0) {
                        wp->cnonce = bstrdup(vp);
                    } else if (gstricmp(cp, T("qop")) == 0) {
                        wp->qop = bstrdup(vp);
                    }
                    /*
                        Restore tag name and value zero-terminations
                     */
                    *np = tp;
                    *npv = tpv;
                    /*
                        Advance tag name and value pointers
                     */
                    cp = npv;
                    while (*cp && isgoodchar(*cp)) {
                        cp++;
                    }
                    while (*cp && !isgoodchar(*cp)) {
                        cp++;
                    }

                    if (*cp) {
                        vp = gstrchr(cp, '=');
                    } else {
                        vp = NULL;
                    }
                }
#endif /* BIT_DIGEST_AUTH */
            }
        } else if (gstrcmp(key, T("content-length")) == 0) {
            wp->clen = gatoi(value);
            if (wp->clen > 0) {
                wp->flags |= WEBS_CLEN;         
                websSetVar(wp, T("CONTENT_LENGTH"), value);
            } else {
                wp->clen = 0;
            }
        } else if (gstrcmp(key, T("content-type")) == 0) {
            websSetVar(wp, T("CONTENT_TYPE"), value);

#if BIT_KEEP_ALIVE
        } else if (gstrcmp(key, T("connection")) == 0) {
            strlower(value);
            if (gstrcmp(value, T("keep-alive")) == 0) {
                wp->flags |= WEBS_KEEP_ALIVE;
            }
#endif
        } else if (gstrcmp(key, T("cookie")) == 0) {
            wp->flags |= WEBS_COOKIE;
            wp->cookie = bstrdup(value);

#if BIT_IF_MODIFIED
        } else if (gstrcmp(key, T("if-modified-since")) == 0) {
            char_t *cmd;
            time_t tip = 0;

            if ((cp = gstrchr(value, ';')) != NULL) {
                *cp = '\0';
            }

            fmtAlloc(&cmd, 64, T("%s"), value);

            if ((wp->since = dateParse(tip, cmd)) != 0) {
                wp->flags |= WEBS_IF_MODIFIED;
            }
            bfreeSafe(cmd);
#endif /* WEBS_IF_MODIFIED_SUPPORT */
        }
    }
}

/*
    Set the variable (CGI) environment for this request. Create variables for all standard CGI variables. Also decode
    the query string and create a variable for each name=value pair.
 */
void websSetEnv(webs_t wp)
{
    char_t  portBuf[8];
    char_t  *keyword, *value, *valCheck, *valNew;

    a_assert(websValid(wp));

    websSetVar(wp, T("QUERY_STRING"), wp->query);
    websSetVar(wp, T("GATEWAY_INTERFACE"), T("CGI/1.1"));
    websSetVar(wp, T("SERVER_HOST"), websHost);
    websSetVar(wp, T("SERVER_NAME"), websHost);
    websSetVar(wp, T("SERVER_URL"), websHostUrl);
    websSetVar(wp, T("REMOTE_HOST"), wp->ipaddr);
    websSetVar(wp, T("REMOTE_ADDR"), wp->ipaddr);
    websSetVar(wp, T("PATH_INFO"), wp->path);
    stritoa(websPort, portBuf, sizeof(portBuf));
    websSetVar(wp, T("SERVER_PORT"), portBuf);
    websSetVar(wp, T("SERVER_ADDR"), wp->ifaddr);
    fmtAlloc(&value, FNAMESIZE, T("GoAhead/%s"), BIT_VERSION);
    websSetVar(wp, T("SERVER_SOFTWARE"), value);
    bfreeSafe(value);
    websSetVar(wp, T("SERVER_PROTOCOL"), wp->protoVersion);

    /*
        Decode and create an environment query variable for each query keyword. We split into pairs at each '&', then
        split pairs at the '='.  Note: we rely on wp->decodedQuery preserving the decoded values in the symbol table.
     */
    wp->decodedQuery = bstrdup(wp->query);
    keyword = gstrtok(wp->decodedQuery, T("&"));
    while (keyword != NULL) {
        if ((value = gstrchr(keyword, '=')) != NULL) {
            *value++ = '\0';
            websDecodeUrl(keyword, keyword, gstrlen(keyword));
            websDecodeUrl(value, value, gstrlen(value));
        } else {
            value = T("");
        }
        if (*keyword) {
            /*
                If keyword has already been set, append the new value to what has been stored.
             */
            if ((valCheck = websGetVar(wp, keyword, NULL)) != 0) {
                fmtAlloc(&valNew, 256, T("%s %s"), valCheck, value);
                websSetVar(wp, keyword, valNew);
                bfreeSafe(valNew);
            } else {
                websSetVar(wp, keyword, value);
            }
        }
        keyword = gstrtok(NULL, T("&"));
    }
}


/*
    Define a webs (CGI) variable for this connection. Also create in relevant scripting engines. Note: the incoming
    value may be volatile.  
 */
void websSetVar(webs_t wp, char_t *var, char_t *value)
{
    value_t      v;

    a_assert(websValid(wp));

    /*
        value_instring will allocate the string if required.
     */
    if (value) {
        v = valueString(value, VALUE_ALLOCATE);
    } else {
        v = valueString(T(""), VALUE_ALLOCATE);
    }
    symEnter(wp->cgiVars, var, v, 0);
}


/*
 *  Return TRUE if a webs variable exists for this connection.
 */
int websTestVar(webs_t wp, char_t *var)
{
    sym_t       *sp;

    a_assert(websValid(wp));

    if (var == NULL || *var == '\0') {
        return 0;
    }
    if ((sp = symLookup(wp->cgiVars, var)) == NULL) {
        return 0;
    }
    return 1;
}


/*
    Get a webs variable but return a default value if string not found.  Note, defaultGetValue can be NULL to permit
    testing existence.  
 */
char_t *websGetVar(webs_t wp, char_t *var, char_t *defaultGetValue)
{
    sym_t   *sp;

    a_assert(websValid(wp));
    a_assert(var && *var);
 
    if ((sp = symLookup(wp->cgiVars, var)) != NULL) {
        a_assert(sp->content.type == string);
        if (sp->content.value.string) {
            return sp->content.value.string;
        } else {
            return T("");
        }
    }
    return defaultGetValue;
}


/*
    Return TRUE if a webs variable is set to a given value
 */
int websCompareVar(webs_t wp, char_t *var, char_t *value)
{
    a_assert(websValid(wp));
    a_assert(var && *var);
 
    if (gstrcmp(value, websGetVar(wp, var, T(" __UNDEF__ "))) == 0) {
        return 1;
    }
    return 0;
}


/*
    Cancel the request timeout. Note may be called multiple times.
 */
void websTimeoutCancel(webs_t wp)
{
    a_assert(websValid(wp));

    if (wp->timeout >= 0) {
        emfUnschedCallback(wp->timeout);
        wp->timeout = -1;
    }
}


/*
    Output a HTTP response back to the browser. If redirect is set to a 
    URL, the browser will be sent to this location.
 */
void websResponse(webs_t wp, int code, char_t *message, char_t *redirect)
{
    char_t      *date;

    a_assert(websValid(wp));

    /*
        IE3.0 needs no Keep Alive for some return codes.
        MOB - OPT REMOVE
     */
    wp->flags &= ~WEBS_KEEP_ALIVE;

    /*
        Only output the header if a header has not already been output.
     */
    if ( !(wp->flags & WEBS_HEADER_DONE)) {
        wp->flags |= WEBS_HEADER_DONE;
        /*
            Redirect behaves much better when sent with HTTP/1.0
         */
        if (redirect != NULL) {
            websWrite(wp, T("HTTP/1.0 %d %s\r\n"), code, websErrorMsg(code));
        } else {
            websWrite(wp, T("HTTP/1.1 %d %s\r\n"), code, websErrorMsg(code));
        }
        /*
            The Server HTTP header below must not be modified unless explicitly allowed by licensing terms.
         */
        websWrite(wp, T("Server: GoAhead/%s\r\n"), BIT_VERSION);

        if ((date = websGetDateString(NULL)) != NULL) {
            websWrite(wp, T("Date: %s\r\n"), date);
            bfree(date);
        }
        /*
            If authentication is required, send the auth header info
         */
        if (code == 401) {
            if (!(wp->flags & WEBS_AUTH_DIGEST)) {
                websWrite(wp, T("WWW-Authenticate: Basic realm=\"%s\"\r\n"), websGetRealm());
#if BIT_DIGEST_AUTH
            } else {
                char_t *nonce, *opaque;

                nonce = websCalcNonce(wp);
                opaque = websCalcOpaque(wp); 
                websWrite(wp, 
                    T("WWW-Authenticate: Digest realm=\"%s\", domain=\"%s\",")
                    T("qop=\"%s\", nonce=\"%s\", opaque=\"%s\",")
                    T("algorithm=\"%s\", stale=\"%s\"\r\n"), 
                    websGetRealm(),
                    websGetHostUrl(),
                    T("auth"),
                    nonce,
                    opaque, T("MD5"), T("FALSE"));
                bfree(nonce);
                bfree(opaque);
#endif
            }
        }

        if (wp->flags & WEBS_KEEP_ALIVE) {
            websWrite(wp, T("Connection: keep-alive\r\n"));
        }
        websWrite(wp, T("Pragma: no-cache\r\nCache-Control: no-cache\r\n"));
        websWrite(wp, T("Content-Type: text/html\r\n"));
        if (redirect) {
            websWrite(wp, T("Location: %s\r\n"), redirect);
        }
        websWrite(wp, T("\r\n"));
    }

    /*
        If the browser didn't do a HEAD only request, send the message as well.
     */
    if ((wp->flags & WEBS_HEAD_REQUEST) == 0 && message && *message) {
        websWrite(wp, T("%s\r\n"), message);
    }
    websDone(wp, code);
}


/*
    Redirect the user to another webs page
 */
void websRedirect(webs_t wp, char_t *url)
{
    char_t  *msgbuf, *urlbuf, *redirectFmt;

    a_assert(websValid(wp));
    a_assert(url);

    websStats.redirects++;
    msgbuf = urlbuf = NULL;

    /*
        Some browsers require a http://host qualified URL for redirection
     */
    if (gstrstr(url, T("http://")) == NULL) {
        if (*url == '/') {
            url++;
        }
        redirectFmt = T("http://%s/%s");

#if BIT_PACK_SSL
        if (wp->flags & WEBS_SECURE) {
            redirectFmt = T("https://%s/%s");
        }
#endif
        fmtAlloc(&urlbuf, WEBS_MAX_URL + 80, redirectFmt, websGetVar(wp, T("HTTP_HOST"),    websHostUrl), url);
        url = urlbuf;
    }

    /*
        Add human readable message for completeness. Should not be required.
     */
    fmtAlloc(&msgbuf, WEBS_MAX_URL + 80, 
        T("<html><head></head><body>\r\n\
        This document has moved to a new <a href=\"%s\">location</a>.\r\n\
        Please update your documents to reflect the new location.\r\n\
        </body></html>\r\n"), url);

    websResponse(wp, 302, msgbuf, url);
    bfreeSafe(msgbuf);
    bfreeSafe(urlbuf);
}


/*
   websSafeUrl -- utility function to clean up URLs that will be printed by the websError() function, below. To prevent
   problems with the 'cross-site scripting exploit', where attackers request an URL containing embedded JavaScript code,
   we replace all '<' and '>' characters with HTML entities so that the user's browser will not interpret the URL as
   JavaScript.
 */

#define kLt '<'
#define kLessThan T("&lt;")
#define kGt '>'
#define kGreaterThan T("&gt;")

static int charCount(const char_t* str, char_t ch)
{
   int count = 0;
   char_t* p = (char_t*) str;
   
    if (NULL == str) {
        return 0;
    }

    while (1) {
        p = gstrchr(p, ch);
        if (NULL == p) {
            break;
        }
        /*
            increment the count, and begin looking at the next character
        */
        ++count;
        ++p;
    }
    return count;
}


//  MOB - review vs appweb

static char_t* websSafeUrl(const char_t* url)
{
    int ltCount = charCount(url, kLt);
    int gtCount = charCount(url, kGt);
    int safeLen = 0;
    char_t* safeUrl = NULL;
    char_t* src = NULL;
    char_t* dest = NULL;

    if (NULL != url) {
        safeLen = gstrlen(url);
        if (ltCount == 0 && gtCount == 0) {
            safeUrl = bstrdup((char_t*) url);
        } else {
            safeLen += (ltCount * 4);
            safeLen += (gtCount * 4);

            safeUrl = balloc(safeLen);
            if (safeUrl != NULL) {
                src = (char_t*) url;
                dest = safeUrl;
                while (*src) {
                    if (*src == kLt) {
                        gstrcpy(dest, kLessThan);
                        dest += gstrlen(kLessThan);
                    } else if (*src == kGt) {
                        gstrcpy(dest, kGreaterThan);
                        dest += gstrlen(kGreaterThan);
                    } else {
                        *dest++ = *src;
                    }
                    ++src;
                }
                /* don't forget to terminate the string...*/
                *dest = '\0';
            }
        }
    }
    return safeUrl;
}



/*  
    Output an error message and cleanup
 */
void websError(webs_t wp, int code, char_t *fmt, ...)
{
    va_list     args;
    char_t      *msg, *userMsg, *buf;
   char_t*     safeUrl = NULL;
   char_t*     safeMsg = NULL;

    a_assert(websValid(wp));
    a_assert(fmt);

    websStats.errors++;

    safeUrl = websSafeUrl(wp->url);
    bfreeSafe(wp->url);
    wp->url = safeUrl;

    va_start(args, fmt);
    userMsg = NULL;
    fmtValloc(&userMsg, WEBS_BUFSIZE, fmt, args);
    va_end(args);
    safeMsg = websSafeUrl(userMsg);
    bfreeSafe(userMsg);
    userMsg = safeMsg;
    safeMsg  = NULL;

    msg = T("<html><head><title>Document Error: %s</title></head>\r\n\
        <body><h2>Access Error: %s</h2>\r\n\
        <p>%s</p></body></html>\r\n");
    buf = NULL;
    fmtAlloc(&buf, WEBS_BUFSIZE, msg, websErrorMsg(code), websErrorMsg(code), userMsg);
    websResponse(wp, code, buf, NULL);
    bfreeSafe(buf);
    bfreeSafe(userMsg);
}


/*
    Return the error message for a given code
 */
char_t *websErrorMsg(int code)
{
    websErrorType   *ep;

    for (ep = websErrors; ep->code; ep++) {
        if (code == ep->code) {
            return ep->msg;
        }
    }
    a_assert(0);
    return T("");
}


/*
    Do formatted output to the browser. This is the public ASP and form write procedure.
 */
int websWrite(webs_t wp, char_t *fmt, ...)
{
    va_list      vargs;
    char_t      *buf;
    int          rc;
    
    a_assert(websValid(wp));

    va_start(vargs, fmt);

    buf = NULL;
    rc = 0;
    if (fmtValloc(&buf, WEBS_BUFSIZE, fmt, vargs) >= WEBS_BUFSIZE) {
        trace(0, T("webs: websWrite lost data, buffer overflow\n"));
    }
    va_end(vargs);
    a_assert(buf);
    if (buf) {
        rc = websWriteBlock(wp, buf, gstrlen(buf));
        bfree(buf);
    }
    return rc;
}


/*
    Write a block of data of length "nChars" to the user's browser. Public write block procedure.  If unicode is turned
    on this function expects buf to be a unicode string and it converts it to ASCII before writing.  See
    websWriteDataNonBlock to always write binary or ASCII data with no unicode conversion.  This returns the number of
    char_t's processed.  It spins until nChars are flushed to the socket.  For non-blocking behavior, use
    websWriteDataNonBlock.
 */
int websWriteBlock(webs_t wp, char_t *buf, int nChars)
{
    int     len, done;
    char    *asciiBuf, *pBuf;

    a_assert(wp);
    a_assert(websValid(wp));
    a_assert(buf);
    a_assert(nChars >= 0);

    done = len = 0;

    /*
        ballocUniToAsc will convert Unicode to strings to Ascii.  If Unicode is
        not turned on then ballocUniToAsc will not do the conversion.
     */
    pBuf = asciiBuf = ballocUniToAsc(buf, nChars);

    while (nChars > 0) {  
#if BIT_PACK_SSL
        if (wp->flags & WEBS_SECURE) {
            if ((len = websSSLWrite(wp->wsp, pBuf, nChars)) < 0) {
                bfree(asciiBuf);
                return -1;
            }
            websSSLFlush(wp->wsp);
        } else {
            if ((len = socketWrite(wp->sid, pBuf, nChars)) < 0) {
                bfree(asciiBuf);
                return -1;
            }
            socketFlush(wp->sid);
        }
#else
        if ((len = socketWrite(wp->sid, pBuf, nChars)) < 0) {
            bfree(asciiBuf);
            return -1;
        }
        socketFlush(wp->sid);
#endif
        nChars -= len;
        pBuf += len;
        done += len;
    }
    bfree(asciiBuf);
    return done;
}


/*
    Write a block of data of length "nChars" to the user's browser. Same as websWriteBlock except that it expects
    straight ASCII or binary and does no unicode conversion before writing the data.  If the socket cannot hold all the
    data, it will return the number of bytes flushed to the socket before it would have blocked.  This returns the
    number of chars processed or -1 if socketWrite fails.
 */
int websWriteDataNonBlock(webs_t wp, char *buf, int nChars)
{
    int r;

    a_assert(wp);
    a_assert(websValid(wp));
    a_assert(buf);
    a_assert(nChars >= 0);

#if BIT_PACK_SSL
    if (wp->flags & WEBS_SECURE) {
        r = websSSLWrite(wp->wsp, buf, nChars);
        websSSLFlush(wp->wsp);
    } else {
        r = socketWrite(wp->sid, buf, nChars);
        socketFlush(wp->sid);
    }
#else
    r = socketWrite(wp->sid, buf, nChars);
    socketFlush(wp->sid);
#endif
    return r;
}


/*
    Decode a URL (or part thereof). Allows insitu decoding.
 */
void websDecodeUrl(char_t *decoded, char_t *token, int len)
{
    char_t  *ip,  *op;
    int     num, i, c;
    
    a_assert(decoded);
    a_assert(token);

    op = decoded;
    for (ip = token; *ip && len > 0; ip++, op++) {
        if (*ip == '+') {
            *op = ' ';
        } else if (*ip == '%' && gisxdigit(ip[1]) && gisxdigit(ip[2])) {
            /*
                Convert %nn to a single character
             */
            ip++;
            for (i = 0, num = 0; i < 2; i++, ip++) {
                c = tolower(*ip);
                if (c >= 'a' && c <= 'f') {
                    num = (num * 16) + 10 + c - 'a';
                } else {
                    num = (num * 16) + c - '0';
                }
            }
            *op = (char_t) num;
            ip--;

        } else {
            *op = *ip;
        }
        len--;
    }
    *op = '\0';
}


#if BIT_ACCESS_LOG
/*
    Output a log message in Common Log Format
        http://httpd.apache.org/docs/1.3/logs.html#common
    If WEBS_SIMPLE_TIME is defined, then only the time() external API is used
        and a simple, non-standard log format is used.
    If WEBS_LOG_QUERY is defined, then the query string will be printed to
        the log, in addition to the URL path.  This can be a security issue
        if the query string contains sensitive information that shouldn't
        be hanging around in log files.
    MOB - get rid of LOG_QUERY
 */
static void websLog(webs_t wp, int code)
{
    char_t  *buf;
    char    *abuf;
    int     len;
#if !WEBS_SIMPLE_TIME
    time_t timer;
    struct tm localt;
#if WIN
    DWORD   dwRet;
    TIME_ZONE_INFORMATION   tzi;
#endif /* WIN */
    char_t timeStr[28];
    char_t zoneStr[6];
    char_t dataStr[16];
#endif
    a_assert(websValid(wp));
    buf = NULL;
#if !WEBS_SIMPLE_TIME
    time(&timer);
    localtime_r(&timer, &localt);
    strftime(timeStr, sizeof(timeStr), "%d/%b/%Y:%H:%M:%S", &localt); 
    timeStr[sizeof(timeStr) - 1] = '\0';
#if WIN
    dwRet = GetTimeZoneInformation(&tzi);
    snprintf(zoneStr, sizeof(zoneStr), "%+03d00", -(int)(tzi.Bias/60));
#else
    snprintf(zoneStr, sizeof(zoneStr), "%+03d00", (int)(localt.tm_gmtoff/3600));
#endif /* WIN */
    zoneStr[sizeof(zoneStr) - 1] = '\0';
    if (wp->written != 0) {
        snprintf(dataStr, sizeof(dataStr), "%d", wp->written);
        dataStr[sizeof(dataStr) - 1] = '\0';
    } else {
        dataStr[0] = '-'; dataStr[1] = '\0';
    }
    fmtAlloc(&buf, WEBS_MAX_URL + 80, 
        T("%s - %s [%s %s] \"%s %s %s\" %d %s\n"), 
        wp->ipaddr,
        wp->userName == NULL ? "-" : wp->userName,
        timeStr, zoneStr,
        wp->flags & WEBS_POST_REQUEST ? "POST" : 
            (wp->flags & WEBS_HEAD_REQUEST ? "HEAD" : "GET"),
#if WEBS_LOG_QUERY
        wp->url, /* SECURITY - Printing the query can 'leak' private data */
#else
        wp->path,
#endif /* WEBS_LOG_QUERY */
        wp->protoVersion, code, dataStr);
#else
    //  MOB - reverse conditional
    fmtAlloc(&buf, WEBS_MAX_URL + 80, T("%d %s %d %d\n"), time(0), wp->url, code, wp->written);
#endif
    len = gstrlen(buf);
    abuf = ballocUniToAsc(buf, len+1);
    write(websLogFd, abuf, len);
    bfreeSafe(buf);
    bfreeSafe(abuf);
}
#endif /* WEBS_LOG_SUPPORT */



#if BIT_DEBUG_TRACE
static void traceHandler(int level, char_t *buf)
{
    int     len;
    char    *abuf;

    if (level <= WEBS_TRACE_LEVEL) {    
        len = gstrlen(buf);
        abuf = ballocUniToAsc(buf, len+1);
        write(websTraceFd, abuf, len);
        bfreeSafe(abuf);
    }
}

#endif /* WEBS_TRACE_SUPPORT */

/*
    Request timeout. The timeout triggers if we have not read any data from the users browser in the last WEBS_TIMEOUT
    period. If we have heard from the browser, simply re-issue the timeout.
 */
void websTimeout(void *arg, int id)
{
    webs_t      wp;
    int         delay, tm;

    wp = (webs_t) arg;
    a_assert(websValid(wp));

    tm = websGetTimeSinceMark(wp) * 1000;
    if (tm >= WEBS_TIMEOUT) {
        websStats.timeouts++;
        emfUnschedCallback(id);
        /*
            Clear the timeout id
         */
        wp->timeout = -1;
        websDone(wp, 404);
    } else {
        delay = WEBS_TIMEOUT - tm;
        a_assert(delay > 0);
        emfReschedCallback(id, delay);
    }
}


/*
    Called when the request is done.
 */
void websDone(webs_t wp, int code)
{
    a_assert(websValid(wp));

    /*
        Disable socket handler in case keep alive set.
     */
    socketDeleteHandler(wp->sid);

    if (code != 200) {
        wp->flags &= ~WEBS_KEEP_ALIVE;
    }
#if BIT_ACCESS_LOG
    if (! (wp->flags & WEBS_REQUEST_DONE)) {
        websLog(wp, code);
    }
#endif

    /*
        Close any opened document by a handler
     */
    websPageClose(wp);

    /*
        Exit if secure
     */
#if BIT_PACK_SSL
    if (wp->flags & WEBS_SECURE) {
        websTimeoutCancel(wp);
        websSSLFlush(wp->wsp);
        socketCloseConnection(wp->sid);
        websFree(wp);
        return;
    }
#endif

    /*
        If using Keep Alive (HTTP/1.1) we keep the socket open for a period while waiting for another request on the socket. 
     */
    if (wp->flags & WEBS_KEEP_ALIVE) {
        if (socketFlush(wp->sid) == 0) {
            wp->state = WEBS_BEGIN;
            wp->flags |= WEBS_REQUEST_DONE;
            if (wp->header.buf) {
                ringqFlush(&wp->header);
            }
            socketCreateHandler(wp->sid, SOCKET_READABLE, websSocketEvent, 
                wp);
            websTimeoutCancel(wp);
            wp->timeout = emfSchedCallback(WEBS_TIMEOUT, websTimeout,
                (void *) wp);
            return;
        }
    } else {
        websTimeoutCancel(wp);
        socketSetBlock(wp->sid, 1);
        socketFlush(wp->sid);
        socketCloseConnection(wp->sid);
    }
    websFree(wp);
}


int websAlloc(int sid)
{
    webs_t      wp;
    int         wid;

    if ((wid = hAllocEntry((void***) &webs, &websMax, sizeof(struct websRec))) < 0) {
        return -1;
    }
    wp = webs[wid];
    wp->wid = wid;
    wp->sid = sid;
    wp->state = WEBS_BEGIN;
    wp->docfd = -1;
    wp->timeout = -1;
    wp->dir = NULL;
    wp->authType = NULL;
    wp->protocol = NULL;
    wp->protoVersion = NULL;
    wp->password = NULL;
    wp->userName = NULL;
#if BIT_DIGEST_AUTH
    wp->realm = NULL;
    wp->nonce = NULL;
    wp->digest = NULL;
    wp->uri = NULL;
    wp->opaque = NULL;
    wp->nc = NULL;
    wp->cnonce = NULL;
    wp->qop = NULL;
#endif
#if BIT_PACK_SSL
    wp->wsp = NULL;
#endif

    ringqOpen(&wp->header, WEBS_HEADER_BUFINC, WEBS_MAX_HEADER);

    /*
        Create storage for the CGI variables. We supply the symbol tables for both the CGI variables and for the global
        functions. The function table is common to all webs instances (ie. all browsers)
     */
    wp->cgiVars = symOpen(WEBS_SYM_INIT);

    return wid;
}


void websFree(webs_t wp)
{
    a_assert(websValid(wp));

    //  MOB _ better if bfree permits a null
    if (wp->path)
        bfree(wp->path);
    if (wp->url)
        bfree(wp->url);
    if (wp->host)
        bfree(wp->host);
    if (wp->lpath)
        bfree(wp->lpath);
    if (wp->query)
        bfree(wp->query);
    if (wp->decodedQuery)
        bfree(wp->decodedQuery);
    if (wp->authType)
        bfree(wp->authType);
    if (wp->password)
        bfree(wp->password);
    if (wp->userName)
        bfree(wp->userName);
    if (wp->cookie)
        bfree(wp->cookie);
    if (wp->userAgent)
        bfree(wp->userAgent);
    if (wp->dir)
        bfree(wp->dir);
    if (wp->protocol)
        bfree(wp->protocol);
    if (wp->protoVersion)
        bfree(wp->protoVersion);
    if (wp->cgiStdin)
        bfree(wp->cgiStdin);

#if BIT_DIGEST_AUTH
    if (wp->realm)
        bfree(wp->realm);
    if (wp->uri)
        bfree(wp->uri);
    if (wp->digest)
        bfree(wp->digest);
    if (wp->opaque)
        bfree(wp->opaque);
    if (wp->nonce)
        bfree(wp->nonce);
    if (wp->nc)
        bfree(wp->nc);
    if (wp->cnonce)
        bfree(wp->cnonce);
    if (wp->qop)
        bfree(wp->qop);
#endif
#if BIT_PACK_SSL
    websSSLFree(wp->wsp);
#endif
    symClose(wp->cgiVars);

    if (wp->header.buf) {
        ringqClose(&wp->header);
    }

    websMax = hFree((void***) &webs, wp->wid);
    bfree(wp);
    a_assert(websMax >= 0);
}


char_t *websGetHost()
{
    return websHost;
}


char_t *websGetIpaddrUrl()
{
    return websIpaddrUrl;
}


char_t *websGetHostUrl()
{
    return websHostUrl;
}


int websGetPort()
{
    return websPort;
}


int websGetRequestBytes(webs_t wp)
{
    a_assert(websValid(wp));
    return wp->numbytes;
}


char_t *websGetRequestDir(webs_t wp)
{
    a_assert(websValid(wp));

    if (wp->dir == NULL) {
        return T("");
    }
    return wp->dir;
}


int websGetRequestFlags(webs_t wp)
{
    a_assert(websValid(wp));

    return wp->flags;
}


char_t *websGetRequestIpaddr(webs_t wp)
{
    a_assert(websValid(wp));

    return wp->ipaddr;
}


char_t *websGetRequestLpath(webs_t wp)
{
    a_assert(websValid(wp));

    //  MOB - unify
#if BIT_ROM
    return wp->path;
#else
    return wp->lpath;
#endif
}


char_t *websGetRequestPath(webs_t wp)
{
    a_assert(websValid(wp));

    if (wp->path == NULL) {
        return T("");
    }
    return wp->path;
}


char_t *websGetRequestPassword(webs_t wp)
{
    a_assert(websValid(wp));
    return wp->password;
}


char_t *websGetRequestType(webs_t wp)
{
    a_assert(websValid(wp));
    return wp->type;
}


char_t *websGetRequestUserName(webs_t wp)
{
    a_assert(websValid(wp));
    return wp->userName;
}


int websGetRequestWritten(webs_t wp)
{
    a_assert(websValid(wp));

    return wp->written;
}


void websSetHost(char_t *host)
{
    gstrncpy(websHost, host, TSZ(websHost));
}


void websSetHostUrl(char_t *url)
{
    a_assert(url && *url);

    bfreeSafe(websHostUrl);
    websHostUrl = gstrdup(url);
}


void websSetIpaddr(char_t *ipaddr)
{
    a_assert(ipaddr && *ipaddr);
    gstrncpy(websIpaddr, ipaddr, TSZ(websIpaddr));
}


void websSetRequestBytes(webs_t wp, int bytes)
{
    a_assert(websValid(wp));
    a_assert(bytes >= 0);
    wp->numbytes = bytes;
}


void websSetRequestFlags(webs_t wp, int flags)
{
    a_assert(websValid(wp));
    wp->flags = flags;
}


void websSetRequestLpath(webs_t wp, char_t *lpath)
{
    a_assert(websValid(wp));
    a_assert(lpath && *lpath);

    if (wp->lpath) {
        bfree(wp->lpath);
    }
    wp->lpath = bstrdup(lpath);
    websSetVar(wp, T("PATH_TRANSLATED"), wp->lpath);
}


/*
    Update the URL path and the directory containing the web page
 */
void websSetRequestPath(webs_t wp, char_t *dir, char_t *path)
{
    char_t  *tmp;

    a_assert(websValid(wp));

    if (dir) { 
        tmp = wp->dir;
        wp->dir = bstrdup(dir);
        if (tmp) {
            bfree(tmp);
        }
    }
    if (path) {
        tmp = wp->path;
        wp->path = bstrdup(path);
        websSetVar(wp, T("PATH_INFO"), wp->path);
        if (tmp) {
            bfree(tmp);
        }
    }
}


/*
    Set the Write handler for this socket
 */
void websSetRequestSocketHandler(webs_t wp, int mask, void (*fn)(webs_t wp))
{
    a_assert(websValid(wp));
    wp->writeSocket = fn;
    socketCreateHandler(wp->sid, SOCKET_WRITABLE, websSocketEvent, wp);
}


void websSetRequestWritten(webs_t wp, int written)
{
    a_assert(websValid(wp));
    wp->written = written;
}


int websValid(webs_t wp)
{
    int     wid;

    for (wid = 0; wid < websMax; wid++) {
        if (wp == webs[wid]) {
            return 1;
        }
    }
    return 0;
}


/*
    Build an ASCII time string.  If sbuf is NULL we use the current time, else we use the last modified time of sbuf;
 */
char_t *websGetDateString(websStatType *sbuf)
{
    char_t* cp, *r;
    time_t  now;

    if (sbuf == NULL) {
        time(&now);
    } else {
        now = sbuf->mtime;
    }
    if ((cp = gctime(&now)) != NULL) {
        cp[gstrlen(cp) - 1] = '\0';
        r = bstrdup(cp);
        return r;
    }
    return NULL;
}


/*
    Mark time. Set a timestamp so that, later, we can return the number of seconds since we made the mark. Note that the
    mark my not be a "real" time, but rather a relative marker.
 */
void websSetTimeMark(webs_t wp)
{
    wp->timestamp = time(0);
}


/*
    Get the number of seconds since the last mark.
 */
static int websGetTimeSinceMark(webs_t wp)
{
    return time(0) - wp->timestamp;
}


/*
    Store the new realm name
 */
void websSetRealm(char_t *realmName)
{
    a_assert(realmName);

    gstrncpy(websRealm, realmName, TSZ(websRealm));
}


/*
    Return the realm name (used for authorization)
 */
char_t *websGetRealm()
{
    return websRealm;
}


#if BIT_IF_MODIFIED
//  MOB - move all into a date.c
/*  
    These functions are intended to closely mirror the syntax for HTTP-date 
    from RFC 2616 (HTTP/1.1 spec).  This code was submitted by Pete Bergstrom.
    
    RFC1123Date = wkday "," SP date1 SP time SP "GMT"
    RFC850Date  = weekday "," SP date2 SP time SP "GMT"
    ASCTimeDate = wkday SP date3 SP time SP 4DIGIT
  
    Each of these functions tries to parse the value and update the index to 
    the point it leaves off parsing.
 */

typedef enum { JAN, FEB, MAR, APR, MAY, JUN, JUL, AUG, SEP, OCT, NOV, DEC } MonthEnumeration;
typedef enum { SUN, MON, TUE, WED, THU, FRI, SAT } WeekdayEnumeration;

/*  
    Parse an N-digit value
 */

static int parseNDIGIT(char_t *buf, int digits, int *index) 
{
    int tmpIndex, returnValue;

    returnValue = 0;
    for (tmpIndex = *index; tmpIndex < *index+digits; tmpIndex++) {
        if (gisdigit(buf[tmpIndex])) {
            returnValue = returnValue * 10 + (buf[tmpIndex] - T('0'));
        }
    }
    *index = tmpIndex;
    return returnValue;
}


/*
    Return an index into the month array
 */

static int parseMonth(char_t *buf, int *index) 
{
    /*  
        "Jan" | "Feb" | "Mar" | "Apr" | "May" | "Jun" | 
        "Jul" | "Aug" | "Sep" | "Oct" | "Nov" | "Dec"
     */
    int tmpIndex, returnValue;
    returnValue = -1;
    tmpIndex = *index;

    switch (buf[tmpIndex]) {
        case 'A':
            switch (buf[tmpIndex+1]) {
                case 'p':
                    returnValue = APR;
                    break;
                case 'u':
                    returnValue = AUG;
                    break;
            }
            break;
        case 'D':
            returnValue = DEC;
            break;
        case 'F':
            returnValue = FEB;
            break;
        case 'J':
            switch (buf[tmpIndex+1]) {
                case 'a':
                    returnValue = JAN;
                    break;
                case 'u':
                    switch (buf[tmpIndex+2]) {
                        case 'l':
                            returnValue = JUL;
                            break;
                        case 'n':
                            returnValue = JUN;
                            break;
                    }
                    break;
            }
            break;
        case 'M':
            switch (buf[tmpIndex+1]) {
                case 'a':
                    switch (buf[tmpIndex+2]) {
                        case 'r':
                            returnValue = MAR;
                            break;
                        case 'y':
                            returnValue = MAY;
                            break;
                    }
                    break;
            }
            break;
        case 'N':
            returnValue = NOV;
            break;
        case 'O':
            returnValue = OCT;
            break;
        case 'S':
            returnValue = SEP;
            break;
    }
    if (returnValue >= 0) {
        *index += 3;
    }
    return returnValue;
}


/* 
    Parse a year value (either 2 or 4 digits)
 */
static int parseYear(char_t *buf, int *index) 
{
    int tmpIndex, returnValue;

    tmpIndex = *index;
    returnValue = parseNDIGIT(buf, 4, &tmpIndex);

    if (returnValue >= 0) {
        *index = tmpIndex;
    } else {
        returnValue = parseNDIGIT(buf, 2, &tmpIndex);
        if (returnValue >= 0) {
            /*
                Assume that any year earlier than the start of the epoch for time_t (1970) specifies 20xx
             */
            if (returnValue < 70) {
                returnValue += 2000;
            } else {
                returnValue += 1900;
            }
            *index = tmpIndex;
        }
    }
    return returnValue;
}


/* 
    The formulas used to build these functions are from "Calendrical Calculations", by Nachum Dershowitz, Edward M.
    Reingold, Cambridge University Press, 1997.  
 */

//  MOB - move to header
#include <math.h>

//  MOB - static
const int GregorianEpoch = 1;


/*
    Determine if year is a leap year
 */
int GregorianLeapYearP(long year) 
{
    int     result;
    long    tmp;
    
    tmp = year % 400;

    if ((year % 4 == 0) && (tmp != 100) && (tmp != 200) && (tmp != 300)) {
        result = TRUE;
    } else {
        result = FALSE;
    }
    return result;
}


/*
    Return the fixed date from the gregorian date
 */
long FixedFromGregorian(long month, long day, long year) 
{
    long fixedDate;

    fixedDate = (long)(GregorianEpoch - 1 + 365 * (year - 1) + 
        floor((year - 1) / 4.0) -
        floor((double)(year - 1) / 100.0) + 
        floor((double)(year - 1) / 400.0) + 
        floor((367.0 * ((double)month) - 362.0) / 12.0));

    if (month <= 2) {
        fixedDate += 0;
    } else if (TRUE == GregorianLeapYearP(year)) {
        fixedDate += -1;
    } else {
        fixedDate += -2;
    }
    fixedDate += day;
    return fixedDate;
}


/*
    Return the gregorian year from a fixed date
 */
long GregorianYearFromFixed(long fixedDate) 
{
    long result, d0, n400, d1, n100, d2, n4, d3, n1, d4, year;

    d0 =    fixedDate - GregorianEpoch;
    n400 =  (long)(floor((double)d0 / (double)146097));
    d1 =    d0 % 146097;
    n100 =  (long)(floor((double)d1 / (double)36524));
    d2 =    d1 % 36524;
    n4 =    (long)(floor((double)d2 / (double)1461));
    d3 =    d2 % 1461;
    n1 =    (long)(floor((double)d3 / (double)365));
    d4 =    (d3 % 365) + 1;
    year =  400 * n400 + 100 * n100 + 4 * n4 + n1;

    if ((n100 == 4) || (n1 == 4)) {
        result = year;
    } else {
        result = year + 1;
    }
    return result;
}


/* 
    Returns the Gregorian date from a fixed date (not needed for this use, but included for completeness)
 */
#if UNUSED && KEEP
GregorianFromFixed(long fixedDate, long *month, long *day, long *year) 
{
    long priorDays, correction;

    *year =         GregorianYearFromFixed(fixedDate);
    priorDays =     fixedDate - FixedFromGregorian(1, 1, *year);

    if (fixedDate < FixedFromGregorian(3,1,*year)) {
        correction = 0;
    } else if (true == GregorianLeapYearP(*year)) {
        correction = 1;
    } else {
        correction = 2;
    }
    *month = (long)(floor((12.0 * (double)(priorDays + correction) + 373.0) / 367.0));
    *day = fixedDate - FixedFromGregorian(*month, 1, *year);
}
#endif


/* 
    Returns the difference between two Gregorian dates
 */
long GregorianDateDifferenc(long month1, long day1, long year1,
                            long month2, long day2, long year2) 
{
    return FixedFromGregorian(month2, day2, year2) - FixedFromGregorian(month1, day1, year1);
}


/*
    Return the number of seconds into the current day
 */
#define SECONDS_PER_DAY 24*60*60

static int parseTime(char_t *buf, int *index) 
{
    /*  
        Format of buf is - 2DIGIT ":" 2DIGIT ":" 2DIGIT
     */
    int returnValue, tmpIndex, hourValue, minuteValue, secondValue;

    hourValue = minuteValue = secondValue = -1;
    returnValue = -1;
    tmpIndex = *index;

    hourValue = parseNDIGIT(buf, 2, &tmpIndex);

    if (hourValue >= 0) {
        tmpIndex++;
        minuteValue = parseNDIGIT(buf, 2, &tmpIndex);
        if (minuteValue >= 0) {
            tmpIndex++;
            secondValue = parseNDIGIT(buf, 2, &tmpIndex);
        }
    }
    if ((hourValue >= 0) && (minuteValue >= 0) && (secondValue >= 0)) {
        returnValue = (((hourValue * 60) + minuteValue) * 60) + secondValue;
        *index = tmpIndex;
    }
    return returnValue;
}


/*
    Return the equivalent of time() given a gregorian date
 */
static time_t dateToTimet(int year, int month, int day) 
{
    long dayDifference;

    dayDifference = FixedFromGregorian(month + 1, day, year) - FixedFromGregorian(1, 1, 1970);
    return dayDifference * SECONDS_PER_DAY;
}


/*
    Return the number of seconds between Jan 1, 1970 and the parsed date (corresponds to documentation for time() function)
 */
static time_t parseDate1or2(char_t *buf, int *index) 
{
    /*  
        Format of buf is either
        2DIGIT SP month SP 4DIGIT
        or
        2DIGIT "-" month "-" 2DIGIT
     */
    int     dayValue, monthValue, yearValue, tmpIndex;
    time_t  returnValue;

    returnValue = (time_t) -1;
    tmpIndex = *index;

    dayValue = monthValue = yearValue = -1;

    if (buf[tmpIndex] == T(',')) {
        /* 
            Skip over the ", " 
         */
        tmpIndex += 2; 

        dayValue = parseNDIGIT(buf, 2, &tmpIndex);
        if (dayValue >= 0) {
            /*
                Skip over the space or hyphen
             */
            tmpIndex++; 
            monthValue = parseMonth(buf, &tmpIndex);
            if (monthValue >= 0) {
                /*
                    Skip over the space or hyphen
                 */
                tmpIndex++; 
                yearValue = parseYear(buf, &tmpIndex);
            }
        }

        if ((dayValue >= 0) &&
            (monthValue >= 0) &&
            (yearValue >= 0)) {
            if (yearValue < 1970) {
                /*              
                    Allow for Microsoft IE's year 1601 dates 
                 */
                returnValue = 0; 
            } else {
                returnValue = dateToTimet(yearValue, monthValue, dayValue);
            }
            *index = tmpIndex;
        }
    }
    
    return returnValue;
}


/*
    Return the number of seconds between Jan 1, 1970 and the parsed date
 */
static time_t parseDate3Time(char_t *buf, int *index) 
{
    /*
        Format of buf is month SP ( 2DIGIT | ( SP 1DIGIT ))
     */
    int     dayValue, monthValue, yearValue, timeValue, tmpIndex;
    time_t  returnValue;

    returnValue = (time_t) -1;
    tmpIndex = *index;

    dayValue = monthValue = yearValue = timeValue = -1;

    monthValue = parseMonth(buf, &tmpIndex);
    if (monthValue >= 0) {
        /*      
            Skip over the space 
         */
        tmpIndex++; 
        if (buf[tmpIndex] == T(' ')) {
            /*
                Skip over this space too 
             */
            tmpIndex++; 
            dayValue = parseNDIGIT(buf, 1, &tmpIndex);
        } else {
            dayValue = parseNDIGIT(buf, 2, &tmpIndex);
        }
        /*      
            Now get the time and time SP 4DIGIT
         */
        timeValue = parseTime(buf, &tmpIndex);
        if (timeValue >= 0) {
            /*          
                Now grab the 4DIGIT year value
             */
            yearValue = parseYear(buf, &tmpIndex);
        }
    }

    if ((dayValue >= 0) && (monthValue >= 0) && (yearValue >= 0)) {
        returnValue = dateToTimet(yearValue, monthValue, dayValue);
        returnValue += timeValue;
        *index = tmpIndex;
    }
    return returnValue;
}


//  MOB - macro and rename
static int bufferIndexIncrementGivenNTest(char_t *buf, int testIndex, char_t testChar, int foundIncrement, int notfoundIncrement) 
{
    if (buf[testIndex] == testChar) {
        return foundIncrement;
    }
    return notfoundIncrement;
}


/*
    Return an index into a logical weekday array
 */
static int parseWeekday(char_t *buf, int *index) 
{
    /*  
        Format of buf is either
            "Mon" | "Tue" | "Wed" | "Thu" | "Fri" | "Sat" | "Sun"
        or
            "Monday" | "Tuesday" | "Wednesday" | "Thursday" | "Friday" | "Saturday" | "Sunday"
     */
    int tmpIndex, returnValue;

    returnValue = -1;
    tmpIndex = *index;

    switch (buf[tmpIndex]) {
        case 'F':
            returnValue = FRI;
            *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'd', sizeof("Friday"), 3);
            break;
        case 'M':
            returnValue = MON;
            *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'd', sizeof("Monday"), 3);
            break;
        case 'S':
            switch (buf[tmpIndex+1]) {
                case 'a':
                    returnValue = SAT;
                    *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'u', sizeof("Saturday"), 3);
                    break;
                case 'u':
                    returnValue = SUN;
                    *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'd', sizeof("Sunday"), 3);
                    break;
            }
            break;
        case 'T':
            switch (buf[tmpIndex+1]) {
                case 'h':
                    returnValue = THU;
                    *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'r', sizeof("Thursday"), 3);
                    break;
                case 'u':
                    returnValue = TUE;
                    *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 's', sizeof("Tuesday"), 3);
                    break;
            }
            break;
        case 'W':
            returnValue = WED;
            *index += bufferIndexIncrementGivenNTest(buf, tmpIndex+3, 'n', sizeof("Wednesday"), 3);
            break;
    }
    return returnValue;
}


/*
        Parse the date and time string.
 */
static time_t dateParse(time_t tip, char_t *cmd)
{
    int index, tmpIndex, weekday, timeValue;
    time_t parsedValue, dateValue;

    parsedValue = (time_t) 0;
    index = timeValue = 0;
    weekday = parseWeekday(cmd, &index);

    if (weekday >= 0) {
        tmpIndex = index;
        dateValue = parseDate1or2(cmd, &tmpIndex);
        if (dateValue >= 0) {
            index = tmpIndex + 1;
            /*
                One of these two forms is being used
                wkday "," SP date1 SP time SP "GMT"
                weekday "," SP date2 SP time SP "GMT"
             */
            timeValue = parseTime(cmd, &index);
            if (timeValue >= 0) {
                /*              
                    Now match up that "GMT" string for completeness
                    Compute the final value if there were no problems in the parse
                 */
                if ((weekday >= 0) &&
                    (dateValue >= 0) &&
                    (timeValue >= 0)) {
                    parsedValue = dateValue + timeValue;
                }
            }
        } else {
            /* 
                Try the other form - wkday SP date3 SP time SP 4DIGIT
             */
            tmpIndex = index;
            parsedValue = parseDate3Time(cmd, &tmpIndex);
        }
    }

    return parsedValue;
}

#endif /* WEBS_IF_MODIFIED_SUPPORT */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis GoAhead open source license or you may acquire 
    a commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
