#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>
#include <X11/cursorfont.h>
#include <dbus/dbus.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

#define APP_NAME "drink-reminder"
#define CFG_DIR ".config/" APP_NAME
#define PID_DIR "/tmp"
#define DEF_INTERVAL 30
#define MIN_INTERVAL 1
#define MAX_INTERVAL 120

static Display *dpy;
static int scr;
static int interval = DEF_INTERVAL;
static time_t last_reminder = 0;
static volatile sig_atomic_t quit_flag = 0;
static volatile sig_atomic_t sig_remind = 0;

static Window reminder_win = None;
static Window config_win = None;
static Window menu_win = None;
static int menu_active_idx = -1;
static int config_focus = 0;
static char input_buf[8] = "";
static int input_len = 0;

static Atom wm_delete_window;
static Atom net_wm_state;
static Atom net_wm_above;
static Atom net_wm_skip_taskbar;
static Atom net_wm_type;
static Atom net_wm_dialog;
static Atom net_active_window;

// Xft fonts & colors
static XftFont *fn, *ft, *fs;
static XftColor xc_text, xc_white, xc_blue, xc_tip;
static unsigned long p_black, p_white, p_blue, p_tip;

static char config_path[512], pid_path[512], autostart_path[1024], autostart_dir[512];
static DBusConnection *dbus_conn = NULL;

// --- Image (loaded from water.png) ---
typedef struct { int w, h; unsigned char *pixels; } Image;
static Image app_img = {0,0,NULL};

static Image load_png(const char *path) {
    Image img = {0,0,NULL};
    FILE *f = fopen(path,"rb"); if (!f) return img;
    unsigned char sig[8]; fread(sig,1,8,f);
    if (png_sig_cmp(sig,0,8)) { fclose(f); return img; }
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
    if (!p) { fclose(f); return img; }
    png_infop inf = png_create_info_struct(p);
    if (!inf) { png_destroy_read_struct(&p,NULL,NULL); fclose(f); return img; }
    if (setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p,&inf,NULL); fclose(f); return img; }
    png_init_io(p,f); png_set_sig_bytes(p,8); png_read_info(p,inf);
    img.w = png_get_image_width(p,inf); img.h = png_get_image_height(p,inf);
    png_byte ct=png_get_color_type(p,inf), bd=png_get_bit_depth(p,inf);
    if (bd==16) png_set_strip_16(p);
    if (ct==PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(p);
    if (ct==PNG_COLOR_TYPE_GRAY&&bd<8) png_set_expand_gray_1_2_4_to_8(p);
    if (png_get_valid(p,inf,PNG_INFO_tRNS)) png_set_tRNS_to_alpha(p);
    if (ct==PNG_COLOR_TYPE_RGB||ct==PNG_COLOR_TYPE_GRAY||ct==PNG_COLOR_TYPE_PALETTE) png_set_filler(p,0xFF,PNG_FILLER_AFTER);
    if (ct==PNG_COLOR_TYPE_GRAY||ct==PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(p);
    png_read_update_info(p,inf);
    img.pixels = malloc(img.w*img.h*4);
    png_bytep *rows = malloc(sizeof(png_bytep)*img.h);
    for (int y=0;y<img.h;y++) rows[y]=img.pixels+y*img.w*4;
    png_read_image(p,rows); free(rows);
    png_destroy_read_struct(&p,&inf,NULL); fclose(f);
    return img;
}

static Image scale_img(Image *s, int nw, int nh) {
    Image d = {nw,nh,NULL}; d.pixels = malloc(nw*nh*4);
    for (int y=0;y<nh;y++) for (int x=0;x<nw;x++) {
        int sx=x*s->w/nw, sy=y*s->h/nh;
        if (sx>=s->w) sx=s->w-1; if (sy>=s->h) sy=s->h-1;
        memcpy(d.pixels+(y*nw+x)*4, s->pixels+(sy*s->w+sx)*4, 4);
    }
    return d;
}

static void set_net_icon(Window w, Image *img) {
    if (!img->pixels) return;
    int n = img->w * img->h;
    unsigned long *data = malloc(sizeof(unsigned long)*(n+2));
    data[0]=img->w; data[1]=img->h;
    for (int i=0;i<n;i++) {
        unsigned char *p = img->pixels + i*4;
        data[i+2] = ((unsigned long)p[3]<<24)|(p[0]<<16)|(p[1]<<8)|p[2];
    }
    Atom a = XInternAtom(dpy,"_NET_WM_ICON",False);
    XChangeProperty(dpy,w,a,XA_CARDINAL,32,PropModeReplace,(unsigned char*)data,n+2);
    free(data);
}

// --- Water drop icon (fallback when PNG not found) ---
#define IW 24
#define IH 24
static const unsigned char icon_bits[IH][3] = {
    {0,0,0},{0,0,0},{0,0,0},{0,0x7e,0},{0,0xff,0},{1,0xff,0x80},
    {3,0xff,0xc0},{3,0xff,0xc0},{7,0xff,0xe0},{7,0xff,0xe0},{7,0xff,0xe0},
    {7,0xff,0xe0},{7,0xff,0xe0},{3,0xff,0xc0},{3,0xff,0xc0},{1,0xff,0x80},
    {1,0xff,0x80},{0,0xff,0},{0,0x7e,0},{0,0x3c,0},{0,0x18,0},
    {0,0,0},{0,0,0},{0,0,0},
};

static const char *S_TITLE = "该喝水啦！";
static const char *S_CONF_TITLE = "喝水提醒 · 设置";
static const char *S_OK = "知道了";
static const char *S_SAVE = "保存";
static const char *S_CANCEL = "取消";
static const char *S_LABEL = "提醒间隔（分钟）";

static void show_reminder(void);
static void hide_reminder(void);
static void show_config(void);
static void hide_config(void);
static void show_menu(void);
static void hide_menu(void);

// ====== Paths / Config / PID ======
static void init_paths(void) {
    struct passwd *pw = getpwuid(getuid());
    char *h = pw ? pw->pw_dir : getenv("HOME");
    if (!h) h = "/tmp";
    snprintf(config_path,512,"%s/%s/config",h,CFG_DIR);
    snprintf(pid_path,512,"%s/%s.pid",PID_DIR,APP_NAME);
    snprintf(autostart_dir,512,"%s/.config/autostart",h);
    snprintf(autostart_path,1024,"%s/%s.desktop",autostart_dir,APP_NAME);
    char d[512]; snprintf(d,512,"%s/%s",h,CFG_DIR);
    mkdir(d,0755); mkdir(autostart_dir,0755);
}
static void load_config(void) {
    FILE *f = fopen(config_path,"r"); if (!f) return;
    int v=DEF_INTERVAL; if (fscanf(f,"interval=%d",&v)==1 && v>=MIN_INTERVAL && v<=MAX_INTERVAL) interval=v;
    fclose(f);
}
static void save_config(void) {
    FILE *f = fopen(config_path,"w"); if (!f) return;
    fprintf(f,"interval=%d\n",interval); fclose(f);
}
static int is_single_instance(void) {
    FILE *f = fopen(pid_path,"r");
    if (f) { pid_t p; if (fscanf(f,"%d",&p)==1 && p>0 && kill(p,0)==0) { fclose(f); return 0; } fclose(f); }
    f = fopen(pid_path,"w"); if (f) { fprintf(f,"%d\n",getpid()); fclose(f); }
    return 1;
}
static void rm_pid(void) { unlink(pid_path); }
static void setup_autostart(void) {
    FILE *f = fopen(autostart_path,"w"); if (!f) return;
    fprintf(f,"[Desktop Entry]\nType=Application\nName=Drink Reminder\nComment=Periodic drink water reminder\nExec=/usr/bin/%s\nTerminal=false\nCategories=Utility;\nX-GNOME-Autostart-enabled=true\n",APP_NAME);
    fclose(f);
}

// ====== Colors & Fonts (Xft) ======
static void init_colors(void) {
    Visual *v = DefaultVisual(dpy, scr);
    Colormap cm = DefaultColormap(dpy, scr);
    p_black = BlackPixel(dpy, scr); p_white = WhitePixel(dpy, scr);
    XftColorAllocName(dpy, v, cm, "#333333", &xc_text);
    XftColorAllocName(dpy, v, cm, "#ffffff", &xc_white);
    XftColorAllocName(dpy, v, cm, "#2196F3", &xc_blue);
    XftColorAllocName(dpy, v, cm, "#999999", &xc_tip);
    // pixel colors for GC drawing (blue, tip)
    {
        XColor c;
        XParseColor(dpy,cm,"#2196F3",&c); XAllocColor(dpy,cm,&c); p_blue = c.pixel;
        XParseColor(dpy,cm,"#999999",&c); XAllocColor(dpy,cm,&c); p_tip = c.pixel;
    }
}
static void init_fonts(void) {
    fn = XftFontOpenName(dpy, scr, "sans-serif:size=13");
    if (!fn) fn = XftFontOpenName(dpy, scr, "fixed:size=13");
    ft = XftFontOpenName(dpy, scr, "sans-serif:size=16:bold");
    if (!ft) ft = XftFontOpenName(dpy, scr, "sans-serif:size=16");
    if (!ft) ft = fn;
    fs = XftFontOpenName(dpy, scr, "sans-serif:size=11");
    if (!fs) fs = XftFontOpenName(dpy, scr, "fixed:size=11");
    if (!fs) fs = fn;
}

// ====== Xft text drawing helpers ======
static void draw_t(Window w, XftFont *f, XftColor *c, int x, int y, const char *s) {
    XftDraw *xd = XftDrawCreate(dpy, w, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
    XftDrawStringUtf8(xd, c, f, x, y + f->ascent, (const unsigned char *)s, strlen(s));
    XftDrawDestroy(xd);
}
static int tw(XftFont *f, const char *s) {
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, f, (const unsigned char *)s, strlen(s), &gi);
    return gi.xOff;
}

// ====== Window helpers ======
static void center_win(Window w, int W, int H) {
    Screen *s = DefaultScreenOfDisplay(dpy);
    XMoveResizeWindow(dpy,w,(s->width-W)/2,(s->height-H)/2,W,H);
}
static void set_above(Window w) {
    XChangeProperty(dpy,w,net_wm_state,XA_ATOM,32,PropModeAppend,(unsigned char*)&net_wm_above,1);
}
static void set_skip(Window w) {
    XChangeProperty(dpy,w,net_wm_state,XA_ATOM,32,PropModeAppend,(unsigned char*)&net_wm_skip_taskbar,1);
}
static void set_type(Window w, Atom t) {
    XChangeProperty(dpy,w,net_wm_type,XA_ATOM,32,PropModeReplace,(unsigned char*)&t,1);
}
static void set_del(Window w) { XSetWMProtocols(dpy,w,&wm_delete_window,1); }
static Window mkwin(int W, int H, int over) {
    XSetWindowAttributes a;
    a.background_pixel = p_white;
    a.event_mask = ExposureMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask|StructureNotifyMask;
    a.override_redirect = over?True:False;
    unsigned long m = CWBackPixel|CWEventMask; if (over) m|=CWOverrideRedirect;
    return XCreateWindow(dpy,DefaultRootWindow(dpy),0,0,W,H,0,CopyFromParent,InputOutput,CopyFromParent,m,&a);
}
static void activate_win(Window w) {
    XEvent e; memset(&e,0,sizeof(e));
    e.xclient.type=ClientMessage; e.xclient.window=w; e.xclient.message_type=net_active_window;
    e.xclient.format=32; e.xclient.data.l[0]=2; e.xclient.data.l[1]=CurrentTime;
    XSendEvent(dpy,DefaultRootWindow(dpy),False,SubstructureRedirectMask|SubstructureNotifyMask,&e);
    XRaiseWindow(dpy,w); XSync(dpy,False); XSetInputFocus(dpy,w,RevertToPointerRoot,CurrentTime);
}
static int xerr_handler(Display *d, XErrorEvent *e) { return 0; }

// ====== DBus SNI ======
static volatile int dbus_remind = 0;
static volatile int dbus_menu = 0;

static void append_icon_pixmap(DBusMessageIter *iter) {
    int w=IW, h=IH;
    unsigned char *pix = NULL;
    if (app_img.pixels) {
        Image sm = scale_img(&app_img, w, h);
        pix = sm.pixels;
    } else {
        pix = malloc(w*h*4);
        for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
            int bi=x/8, bj=7-(x%8), on=(y<h-2&&bi<3)?((icon_bits[y][bi]>>bj)&1):0, i=(y*w+x)*4;
            if (on) { pix[i+0]=0xFF; pix[i+1]=0x99; pix[i+2]=0x33; pix[i+3]=0xFF; }
            else    { pix[i+0]=0;    pix[i+1]=0;    pix[i+2]=0;    pix[i+3]=0;    }
        }
    }
    DBusMessageIter arr, st, ba;
    dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "(iiay)", &arr);
    dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
    dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &w);
    dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &h);
    dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "y", &ba);
    for (int i=0; i<w*h*4; i++) dbus_message_iter_append_basic(&ba, DBUS_TYPE_BYTE, &pix[i]);
    dbus_message_iter_close_container(&st, &ba);
    dbus_message_iter_close_container(&arr, &st);
    dbus_message_iter_close_container(iter, &arr);
    free(pix);
}

static int dbus_append_prop(DBusMessageIter *iter, const char *name) {
    DBusMessageIter v;
    if (!strcmp(name,"Category")) {
        const char *val="Application";
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"Id")) {
        const char *val="drink-reminder";
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"Title")) {
        const char *val="Drink Reminder";
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"Status")) {
        const char *val="Active";
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"WindowId")) {
        dbus_int32_t val=0;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"ItemIsMenu")) {
        dbus_bool_t val=FALSE;
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"Menu")) {
        const char *val="";
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(iter, &v);
    } else if (!strcmp(name,"IconPixmap")) {
        dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "a(iiay)", &v);
        append_icon_pixmap(&v);
        dbus_message_iter_close_container(iter, &v);
    } else return 0;
    return 1;
}

static DBusHandlerResult tray_handler(DBusConnection *conn, DBusMessage *msg, void *ud) {
    if (dbus_message_is_method_call(msg,"org.freedesktop.DBus.Properties","Get")) {
        const char *iface=NULL, *prop=NULL;
        if (!dbus_message_get_args(msg,NULL,DBUS_TYPE_STRING,&iface,DBUS_TYPE_STRING,&prop,DBUS_TYPE_INVALID))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        if (strcmp(iface,"org.kde.StatusNotifierItem")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        DBusMessage *r = dbus_message_new_method_return(msg);
        DBusMessageIter it; dbus_message_iter_init_append(r,&it);
        if (dbus_append_prop(&it,prop)) { dbus_connection_send(conn,r,NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED; }
        dbus_message_unref(r); return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.freedesktop.DBus.Properties","GetAll")) {
        const char *iface=NULL;
        if (!dbus_message_get_args(msg,NULL,DBUS_TYPE_STRING,&iface,DBUS_TYPE_INVALID))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        if (strcmp(iface,"org.kde.StatusNotifierItem")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        DBusMessage *r = dbus_message_new_method_return(msg);
        DBusMessageIter it, d, e;
        dbus_message_iter_init_append(r,&it);
        dbus_message_iter_open_container(&it,DBUS_TYPE_ARRAY,"{sv}",&d);
        const char *props[] = {"Category","Id","Title","Status","WindowId","ItemIsMenu","Menu","IconPixmap",NULL};
        for (int i=0;props[i];i++) {
            dbus_message_iter_open_container(&d,DBUS_TYPE_DICT_ENTRY,NULL,&e);
            const char *k=props[i]; dbus_message_iter_append_basic(&e,DBUS_TYPE_STRING,&k);
            dbus_append_prop(&e,props[i]);
            dbus_message_iter_close_container(&d,&e);
        }
        dbus_message_iter_close_container(&it,&d);
        dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.kde.StatusNotifierItem","Activate")) {
        dbus_menu = 1;
        DBusMessage *r=dbus_message_new_method_return(msg); dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.kde.StatusNotifierItem","SecondaryActivate")) {
        dbus_menu = 1;
        DBusMessage *r=dbus_message_new_method_return(msg); dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.kde.StatusNotifierItem","ContextMenu")) {
        dbus_menu = 1;
        DBusMessage *r=dbus_message_new_method_return(msg); dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.kde.StatusNotifierItem","ProvideXdgActivationToken")) {
        DBusMessage *r=dbus_message_new_method_return(msg); dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg,"org.freedesktop.DBus.Introspectable","Introspect")) {
        const char *xml=
            "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
            "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
            "<node>\n"
            " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
            "   <method name=\"Introspect\"><arg name=\"data\" direction=\"out\" type=\"s\"/></method>\n"
            " </interface>\n"
            " <interface name=\"org.freedesktop.DBus.Properties\">\n"
            "   <method name=\"Get\">"
            "     <arg name=\"interface\" direction=\"in\" type=\"s\"/>"
            "     <arg name=\"property\" direction=\"in\" type=\"s\"/>"
            "     <arg name=\"value\" direction=\"out\" type=\"v\"/>"
            "   </method>\n"
            "   <method name=\"GetAll\">"
            "     <arg name=\"interface\" direction=\"in\" type=\"s\"/>"
            "     <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>"
            "   </method>\n"
            " </interface>\n"
            " <interface name=\"org.kde.StatusNotifierItem\">\n"
            "   <method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
            "   <method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
            "   <method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
            " </interface>\n"
            "</node>\n";
        DBusMessage *r=dbus_message_new_method_return(msg);
        const char *p=xml; dbus_message_append_args(r,DBUS_TYPE_STRING,&p,DBUS_TYPE_INVALID);
        dbus_connection_send(conn,r,NULL); dbus_message_unref(r);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable tray_vtable = { .unregister_function=NULL, .message_function=tray_handler };

static void tray_init_sni(void) {
    DBusError err; dbus_error_init(&err);
    dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) { dbus_error_free(&err); dbus_conn=NULL; return; }
    dbus_connection_set_exit_on_disconnect(dbus_conn, FALSE);
    if (!dbus_connection_register_object_path(dbus_conn, "/StatusNotifierItem", &tray_vtable, NULL)) {
        dbus_connection_unref(dbus_conn); dbus_conn=NULL; return;
    }
    const char *name = dbus_bus_get_unique_name(dbus_conn);
    DBusMessage *msg = dbus_message_new_method_call("org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher","org.kde.StatusNotifierWatcher","RegisterStatusNotifierItem");
    if (msg) {
        dbus_message_append_args(msg,DBUS_TYPE_STRING,&name,DBUS_TYPE_INVALID);
        dbus_connection_send(dbus_conn,msg,NULL);
        dbus_connection_flush(dbus_conn);
        dbus_message_unref(msg);
    }
}

// ====== Menu ======
#define M_W 140
#define M_H 115
#define M_IH 35
#define M_N 3
static const char *m_lbl[M_N] = {"立即提醒","设置","退出"};

static void draw_menu(void) {
    GC g=XCreateGC(dpy,menu_win,0,NULL);
    XSetForeground(dpy,g,p_white); XFillRectangle(dpy,menu_win,g,0,0,M_W,M_H);
    XSetForeground(dpy,g,0xdddddd); XDrawRectangle(dpy,menu_win,g,0,0,M_W-1,M_H-1);
    for (int i=0;i<M_N;i++) {
        int y=2+i*M_IH;
        if (menu_active_idx==i) {
            XSetForeground(dpy,g,p_blue); XFillRectangle(dpy,menu_win,g,2,y,M_W-4,M_IH-4);
            draw_t(menu_win,fn,&xc_white,10,y+(M_IH-fn->ascent-fn->descent)/2,m_lbl[i]);
        } else draw_t(menu_win,fn,&xc_text,10,y+(M_IH-fn->ascent-fn->descent)/2,m_lbl[i]);
        if (i<M_N-1) { XSetForeground(dpy,g,0xeeeeee); XDrawLine(dpy,menu_win,g,5,y+M_IH,M_W-5,y+M_IH); }
    }
    XFreeGC(dpy,g);
}
static void show_menu(void) {
    if (menu_win!=None) return;
    Window r; int rx,ry,wx,wy; unsigned int mk;
    XQueryPointer(dpy,DefaultRootWindow(dpy),&r,&r,&rx,&ry,&wx,&wy,&mk);
    int mx=rx, my=ry; Screen *s=DefaultScreenOfDisplay(dpy);
    if (mx+M_W>s->width) mx=s->width-M_W-5;
    if (my+M_H>s->height) my=s->height-M_H-5;
    menu_win=mkwin(M_W,M_H,1); XMapWindow(dpy,menu_win); XRaiseWindow(dpy,menu_win);
    XMoveWindow(dpy,menu_win,mx,my);
    if (XGrabPointer(dpy,menu_win,True,ButtonPressMask|ButtonReleaseMask|PointerMotionMask,
                     GrabModeAsync,GrabModeAsync,menu_win,None,CurrentTime)!=GrabSuccess)
    { hide_menu(); return; }
    if (XGrabKeyboard(dpy,menu_win,True,GrabModeAsync,GrabModeAsync,CurrentTime)!=GrabSuccess)
    { XUngrabPointer(dpy,CurrentTime); hide_menu(); return; }
    draw_menu();
}
static void hide_menu(void) {
    if (menu_win!=None) {
        XUngrabPointer(dpy,CurrentTime); XUngrabKeyboard(dpy,CurrentTime);
        XDestroyWindow(dpy,menu_win); menu_win=None; menu_active_idx=-1;
    }
}
static int m_hit(int x, int y) {
    if (x<0||x>=M_W||y<2||y>=M_H-2) return -1;
    int i=(y-2)/M_IH; return (i<0||i>=M_N)?-1:i;
}

// ====== Reminder ======
#define R_W 340
#define R_H 210
static void draw_reminder(void) {
    GC g=XCreateGC(dpy,reminder_win,0,NULL);
    XSetForeground(dpy,g,p_white); XFillRectangle(dpy,reminder_win,g,0,0,R_W,R_H);
    // Draw icon (from water.png or fallback bitmap)
    if (app_img.pixels) {
        Image sm = scale_img(&app_img, 48, 48);
        XImage *xi = XCreateImage(dpy,DefaultVisual(dpy,scr),32,ZPixmap,0,
                                  (char*)sm.pixels,48,48,32,0);
        int ix=(R_W-48)/2;
        XPutImage(dpy,reminder_win,g,xi,0,0,ix,15,48,48);
        xi->data = NULL;
        XDestroyImage(xi);
        free(sm.pixels);
    } else {
        int ix=(R_W-IW)/2; XSetForeground(dpy,g,0x3399FF);
        for (int y=0;y<IH-2;y++) for (int x=0;x<3;x++) for (int b=0;b<8;b++)
            if (icon_bits[y][x]&(1<<(7-b))) { int px=ix+x*8+b, py=15+y; XDrawPoint(dpy,reminder_win,g,px,py); }
    }
    draw_t(reminder_win,ft,&xc_text,(R_W-tw(ft,S_TITLE))/2,50,S_TITLE);
    char tip[128]; snprintf(tip,128,"每隔 %d 分钟提醒一次 · 保持健康", interval);
    draw_t(reminder_win,fs,&xc_tip,(R_W-tw(fs,tip))/2,80,tip);
    int bw=120,bh=34,bx=(R_W-bw)/2,by=R_H-bh-20;
    XSetForeground(dpy,g,p_blue); XFillRectangle(dpy,reminder_win,g,bx,by,bw,bh);
    draw_t(reminder_win,fn,&xc_white,(R_W-tw(fn,S_OK))/2,by+(bh-fn->ascent-fn->descent)/2,S_OK);
    XFreeGC(dpy,g);
}
static void show_reminder(void) {
    if (reminder_win!=None) return;
    reminder_win=mkwin(R_W,R_H,0); set_above(reminder_win); set_skip(reminder_win);
    set_type(reminder_win,net_wm_dialog); set_del(reminder_win);
    center_win(reminder_win,R_W,R_H); XMapWindow(dpy,reminder_win); XFlush(dpy);
    set_net_icon(reminder_win, &app_img);
    activate_win(reminder_win); draw_reminder();
    last_reminder=time(NULL);
}
static void hide_reminder(void) {
    if (reminder_win!=None) { XDestroyWindow(dpy,reminder_win); reminder_win=None; }
}

// ====== Config ======
#define C_W 300
#define C_H 210
static void draw_config(void) {
    GC g=XCreateGC(dpy,config_win,0,NULL);
    XSetForeground(dpy,g,p_white); XFillRectangle(dpy,config_win,g,0,0,C_W,C_H);
    draw_t(config_win,ft,&xc_text,(C_W-tw(ft,S_CONF_TITLE))/2,25,S_CONF_TITLE);
    draw_t(config_win,fn,&xc_text,30,60,S_LABEL);
    XSetForeground(dpy,g,p_white); XFillRectangle(dpy,config_win,g,30,88,240,32);
    XSetForeground(dpy,g,config_focus?0x2196F3:0xcccccc);
    XDrawRectangle(dpy,config_win,g,30,88,240,32);
    char d[16]=""; if (input_len>0) strcpy(d,input_buf); else if (config_focus==1) d[0]='|',d[1]=0;
    draw_t(config_win,fn,&xc_text,38,88+(32-fn->ascent-fn->descent)/2,d);
    int bw=(240-10)/2,bh=34,by=C_H-bh-20;
    XSetForeground(dpy,g,p_blue); XFillRectangle(dpy,config_win,g,30,by,bw,bh);
    draw_t(config_win,fn,&xc_white,30+(bw-tw(fn,S_SAVE))/2,by+(bh-fn->ascent-fn->descent)/2,S_SAVE);
    XSetForeground(dpy,g,0xeeeeee); XFillRectangle(dpy,config_win,g,30+bw+10,by,bw,bh);
    XSetForeground(dpy,g,0xcccccc); XDrawRectangle(dpy,config_win,g,30+bw+10,by,bw,bh);
    draw_t(config_win,fn,&xc_text,30+bw+10+(bw-tw(fn,S_CANCEL))/2,by+(bh-fn->ascent-fn->descent)/2,S_CANCEL);
    XFreeGC(dpy,g);
}
static void show_config(void) {
    hide_menu(); if (config_win!=None) { activate_win(config_win); return; }
    snprintf(input_buf,8,"%d",interval); input_len=strlen(input_buf); config_focus=1;
    config_win=mkwin(C_W,C_H,0); set_type(config_win,net_wm_dialog); set_del(config_win);
    center_win(config_win,C_W,C_H); XMapWindow(dpy,config_win); XFlush(dpy);
    set_net_icon(config_win, &app_img);
    activate_win(config_win); draw_config();
}
static void hide_config(void) {
    if (config_win!=None) { XDestroyWindow(dpy,config_win); config_win=None; config_focus=0; }
}
static void config_save(void) {
    int v=interval; if (input_len>0) v=atoi(input_buf);
    if (v<MIN_INTERVAL) v=MIN_INTERVAL; if (v>MAX_INTERVAL) v=MAX_INTERVAL;
    if (v!=interval) { interval=v; save_config(); }
    hide_config();
}

// ====== Event handlers ======
static void h_reminder_click(int x, int y) {
    int bw=120,bh=34,bx=(R_W-bw)/2,by=R_H-bh-20;
    if (x>=bx&&x<=bx+bw&&y>=by&&y<=by+bh) hide_reminder();
}
static void h_config_click(int x, int y) {
    int bw=(240-10)/2,bh=34,by=C_H-bh-20;
    if (x>=30&&x<=30+bw&&y>=by&&y<=by+bh) { config_save(); return; }
    if (x>=30+bw+10&&x<=30+bw+10+bw&&y>=by&&y<=by+bh) { hide_config(); return; }
    config_focus=(x>=30&&x<=270&&y>=88&&y<=120)?1:0; draw_config();
}
static void handle_expose(XExposeEvent *e) {
    if (e->window==menu_win) draw_menu();
    else if (e->window==reminder_win) draw_reminder();
    else if (e->window==config_win) draw_config();
}
static void handle_button(XButtonEvent *e) {
    if (menu_win!=None) {
        if (e->window==menu_win && e->button==Button1) {
            int i=m_hit(e->x,e->y);
            if (i>=0) { hide_menu();
                if (i==0) show_reminder(); else if (i==1) show_config(); else if (i==2) quit_flag=1; }
        } else hide_menu();
        return;
    }
    if (e->window==reminder_win&&e->button==Button1) { h_reminder_click(e->x,e->y); return; }
    if (e->window==config_win&&e->button==Button1) { h_config_click(e->x,e->y); return; }
}
static void handle_motion(XMotionEvent *e) {
    if (e->window==menu_win) { int i=m_hit(e->x,e->y); if (i!=menu_active_idx) { menu_active_idx=i; draw_menu(); } }
}
static void handle_key(XKeyEvent *e) {
    if (e->window==config_win&&config_focus) {
        KeySym ks; char buf[8]=""; XLookupString(e,buf,7,&ks,NULL);
        if (ks==XK_Return||ks==XK_KP_Enter) { config_save(); return; }
        if (ks==XK_Escape) { hide_config(); return; }
        if (ks==XK_BackSpace&&input_len>0) { input_buf[--input_len]=0; draw_config(); return; }
        if (buf[0]>='0'&&buf[0]<='9'&&input_len<6) { input_buf[input_len++]=buf[0]; input_buf[input_len]=0; draw_config(); return; }
        return;
    }
    if (e->window==reminder_win) { return; }
    if (menu_win!=None) { KeySym ks; char b[8]; XLookupString(e,b,8,&ks,NULL); if (ks==XK_Escape) hide_menu(); return; }
}
static void handle_client(XClientMessageEvent *e) {
    if ((Atom)e->data.l[0]==wm_delete_window) {
        if (e->window==reminder_win) hide_reminder(); else if (e->window==config_win) hide_config();
    }
}

// ====== Signal ======
static void on_signal(int sig) {
    if (sig==SIGUSR1) sig_remind=1; else if (sig==SIGINT||sig==SIGTERM) quit_flag=1;
}

// ====== Cleanup ======
static void cleanup(void) {
    if (app_img.pixels) { free(app_img.pixels); app_img.pixels=NULL; }
    if (menu_win!=None) XDestroyWindow(dpy,menu_win);
    if (reminder_win!=None) XDestroyWindow(dpy,reminder_win);
    if (config_win!=None) XDestroyWindow(dpy,config_win);
    if (dbus_conn) { dbus_connection_unref(dbus_conn); dbus_conn=NULL; }
    if (fn) XftFontClose(dpy,fn);
    if (ft&&ft!=fn) XftFontClose(dpy,ft);
    if (fs&&fs!=fn) XftFontClose(dpy,fs);
    {
        Visual *v=DefaultVisual(dpy,scr); Colormap cm=DefaultColormap(dpy,scr);
        XftColorFree(dpy,v,cm,&xc_text); XftColorFree(dpy,v,cm,&xc_white);
        XftColorFree(dpy,v,cm,&xc_blue); XftColorFree(dpy,v,cm,&xc_tip);
    }
    rm_pid(); if (dpy) XCloseDisplay(dpy);
}

// ====== Main ======
int main(int argc, char **argv) {
    if (argc==2 && strncmp(argv[1],"--set",5)==0) {
        init_paths(); load_config();
        char *eq=strchr(argv[1],'='); if (eq) { int v=atoi(eq+1); if (v>=MIN_INTERVAL&&v<=MAX_INTERVAL) { interval=v; save_config(); printf("interval=%d\n",interval); } }
        return 0;
    }
    signal(SIGUSR1,on_signal); signal(SIGINT,on_signal); signal(SIGTERM,on_signal);
    init_paths();
    if (!is_single_instance()) { fprintf(stderr,"Already running\n"); return 1; }
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr,"Cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    init_colors(); init_fonts();
    // Load app icon
    app_img = load_png("water.png");
    if (!app_img.pixels) app_img = load_png("/usr/share/icons/hicolor/256x256/apps/drink-reminder.png");
    wm_delete_window=XInternAtom(dpy,"WM_DELETE_WINDOW",False);
    net_wm_state=XInternAtom(dpy,"_NET_WM_STATE",False);
    net_wm_above=XInternAtom(dpy,"_NET_WM_STATE_ABOVE",False);
    net_wm_skip_taskbar=XInternAtom(dpy,"_NET_WM_STATE_SKIP_TASKBAR",False);
    net_wm_type=XInternAtom(dpy,"_NET_WM_WINDOW_TYPE",False);
    net_wm_dialog=XInternAtom(dpy,"_NET_WM_WINDOW_TYPE_DIALOG",False);
    net_active_window=XInternAtom(dpy,"_NET_ACTIVE_WINDOW",False);
    load_config(); setup_autostart();
    XSetErrorHandler(xerr_handler);
    tray_init_sni();
    last_reminder = time(NULL);
    show_reminder();
    while (!quit_flag) {
        if (sig_remind) { sig_remind=0; show_reminder(); }
        if (dbus_remind) { dbus_remind=0; show_reminder(); }
        if (dbus_menu) { dbus_menu=0; show_menu(); }
        time_t now = time(NULL);
        if (!reminder_win && (now-last_reminder)>=interval*60) show_reminder();
        while (XPending(dpy)>0) {
            XEvent ev; XNextEvent(dpy,&ev);
            switch (ev.type) {
                case Expose: handle_expose(&ev.xexpose); break;
                case ButtonPress: handle_button(&ev.xbutton); break;
                case MotionNotify: handle_motion(&ev.xmotion); break;
                case KeyPress: handle_key(&ev.xkey); break;
                case ClientMessage: handle_client(&ev.xclient); break;
            }
        }
        if (dbus_conn) {
            dbus_connection_read_write(dbus_conn, 0);
            while (dbus_connection_dispatch(dbus_conn) == DBUS_DISPATCH_DATA_REMAINS);
        }
        usleep(100000);
    }
    cleanup();
    return 0;
}
