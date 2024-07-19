#include "server.h"
#include "script.h"
#include "fpconv_dtoa.h"
#include "resp_parser.h"

#include <quickjs.h>

struct jsCtx
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global;
    client *js_client;
} jsctx;

static robj **js_argv = NULL;
static int js_argv_size = 0;


static JSValue redisProtocolToJsType(JSContext *ctx, char* reply) {

}

static JSValue jsConsoleLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int i;
    const char *str;
    double val;
    for (i = 0; i < argc; i++) {
        if(JS_IsNumber(argv[i])) {
            JS_ToFloat64(ctx, &val, argv[i]);
            serverLog(LL_WARNING, "%d", val);
        }else{
            str = JS_ToCString(ctx, argv[i]);
            serverLog(LL_WARNING, "%d", val);
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static robj **jsArgsToRedisArgv(JSContext *ctx, int *argc, JSValueConst *argv) {
    if (js_argv_size < *argc) {
        js_argv = zrealloc(js_argv, sizeof(robj*)* *argc);
        js_argv_size = *argc;
    }

    for (int i = 0; i < *argc; i++) {
        char *obj_s;
        size_t obj_len;
        JSValue jsv = argv[i];
        char buf[64];

        if (JS_IsNumber(argv[i])) {
            double num;
            JS_ToFloat64(ctx, &num, argv[i]);
            long long lvalue;
            if (double2ll(num, &lvalue))
                obj_len = ll2string(buf, sizeof(buf), lvalue);
            else {
                obj_len = fpconv_dtoa(num, buf);
                buf[obj_len] = '\0';
            }
            obj_s = buf;
        } else if (JS_IsString(argv[i])) {
            obj_s = JS_ToCString(ctx, argv[i]);
            if (obj_s == NULL) break; /* Not a string. */
            obj_len = strlen(obj_s);
        } else {
            return NULL;
        }

        js_argv[i] = createStringObject(obj_s, obj_len);
    }
    return js_argv;
}

static JSValue jsRedisCallCommand(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    scriptRunCtx *rctx = JS_GetContextOpaque(ctx);
    client *c = rctx->c;
    sds err = NULL;
    c->argv = jsArgsToRedisArgv(ctx, &argc, argv);
    if (c->argv == NULL) 
        return JS_ThrowInternalError(ctx, "argv must be string or number");
    c->argc = argc;
    c->argv_len = argc;

    sds reply;
    scriptCall(rctx, &err);
    if (err) 
        goto cleanup;
    
    if (listLength(c->reply) == 0 && (size_t)c->bufpos < c->buf_usable_size) {
        c->buf[c->bufpos] = '\0';
        reply = c->buf;
        c->bufpos = 0;
    } else {
        reply = sdsnewlen(c->buf,c->bufpos);
        c->bufpos = 0;
        while(listLength(c->reply)) {
            clientReplyBlock *o = listNodeValue(listFirst(c->reply));
            reply = sdscatlen(reply,o->buf,o->used);
            listDelNode(c->reply,listFirst(c->reply));
        }
    }
    if (reply != c->buf) sdsfree(reply);
    c->reply_bytes = 0;

cleanup:
    for (int i = 0; i < c->argc; i++) 
        JS_FreeCString(ctx, c->argv[i]);
    c->argc = c->argv_len = 0;
    c->user = NULL;
    c->argv = NULL;
    resetClient(c);
    if (err) {
        serverLog(LL_WARNING, err);
        JSValue jsErr = JS_ThrowInternalError(ctx, err);
        sdsfree(err);
        return jsErr;
    }
    serverLog(LL_WARNING, reply);
    return JS_UNDEFINED;
}

void jsRuntimeInit(void) {
    jsctx.js_client = createClient(NULL);
    jsctx.js_client->flags |= CLIENT_SCRIPT;
    jsctx.js_client->flags |= CLIENT_DENY_BLOCKING;
    
    jsctx.rt = JS_NewRuntime();
    jsctx.ctx = JS_NewContext(jsctx.rt);
    if (!jsctx.ctx) {
        JS_FreeRuntime(jsctx.ctx);
        serverLog(LL_WARNING, "Failed creating the quickjs VM.");
        exit(1);
    }
    jsctx.global = JS_GetGlobalObject(jsctx.ctx);
    JSValue redisObj = JS_NewObject(jsctx.ctx);
    JSValue console = JS_NewObject(jsctx.ctx);

    // set global redis methods
    JS_SetPropertyStr(jsctx.ctx, jsctx.global, "redis", redisObj);
    JS_SetPropertyStr(jsctx.ctx, redisObj, "call", JS_NewCFunction(jsctx.ctx, jsRedisCallCommand, "call", 1));
    JS_SetPropertyStr(jsctx.ctx, jsctx.global, "console", console);
    JS_SetPropertyStr(jsctx.ctx, console, "log", JS_NewCFunction(jsctx.ctx, jsConsoleLog, "log", 1));
}

void jsEvalCommand(client *c) {
    long long numkeys;

    /* Get the number of arguments that are keys */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&numkeys,NULL) != C_OK)
        return;
    if (numkeys > (c->argc - 3)) {
        addReplyError(c,"Number of keys can't be greater than number of args");
        return;
    } else if (numkeys < 0) {
        addReplyError(c,"Number of keys can't be negative");
        return;
    }

    scriptRunCtx rctx;
    if (scriptPrepareForRun(&rctx, jsctx.js_client, c, "", 0, 0) != C_OK) {
        return;
    }
    JS_SetContextOpaque(jsctx.ctx, &rctx);
    JS_Eval(jsctx.ctx, c->argv[1]->ptr, strlen(c->argv[1]->ptr), "<input>", JS_EVAL_TYPE_GLOBAL);
    scriptResetRun(&rctx);
    addReply(c,shared.ok);
}
