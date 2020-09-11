/*
 * 2020 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "wine/test.h"

#include <windows.h>

static HRESULT (WINAPI *pCreatePseudoConsole)(COORD,HANDLE,HANDLE,DWORD,HPCON*);
static void (WINAPI *pClosePseudoConsole)(HPCON);

static char console_output[4096];
static unsigned int console_output_count;
static HANDLE console_pipe;
static HANDLE child_pipe;

#define fetch_console_output() fetch_console_output_(__LINE__)
static void fetch_console_output_(unsigned int line)
{
    OVERLAPPED o;
    DWORD count;
    BOOL ret;

    if (console_output_count == sizeof(console_output)) return;

    o.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ret = ReadFile(console_pipe, console_output + console_output_count,
                   sizeof(console_output) - console_output_count, NULL, &o);
    if (!ret)
    {
        ok_(__FILE__,line)(GetLastError() == ERROR_IO_PENDING, "read failed: %u\n", GetLastError());
        if (GetLastError() != ERROR_IO_PENDING) return;
        WaitForSingleObject(o.hEvent, 1000);
    }
    ret = GetOverlappedResult(console_pipe, &o, &count, FALSE);
    if (!ret && GetLastError() == ERROR_IO_PENDING)
        CancelIoEx(console_pipe, &o);

    ok_(__FILE__,line)(ret, "Read file failed: %u\n", GetLastError());
    CloseHandle(o.hEvent);
    if (ret) console_output_count += count;
}

#define expect_empty_output() expect_empty_output_(__LINE__)
static void expect_empty_output_(unsigned int line)
{
    DWORD avail;
    BOOL ret;

    ret = PeekNamedPipe(console_pipe, NULL, 0, NULL, &avail, NULL);
    ok_(__FILE__,line)(ret, "PeekNamedPipe failed: %u\n", GetLastError());
    ok_(__FILE__,line)(!avail, "avail = %u\n", avail);
    if (avail) fetch_console_output_(line);
    ok_(__FILE__,line)(!console_output_count, "expected empty buffer, got %s\n",
                       wine_dbgstr_an(console_output, console_output_count));
    console_output_count = 0;
}

#define expect_output_sequence(a) expect_output_sequence_(__LINE__,0,a)
#define expect_output_sequence_ctx(a,b) expect_output_sequence_(__LINE__,a,b)
static void expect_output_sequence_(unsigned int line, unsigned ctx, const char *expect)
{
    size_t len = strlen(expect);
    if (console_output_count < len) fetch_console_output_(line);
    if (len <= console_output_count && !memcmp(console_output, expect, len))
    {
        console_output_count -= len;
        memmove(console_output, console_output + len, console_output_count);
    }
    else ok_(__FILE__,line)(0, "%x: expected %s got %s\n", ctx, wine_dbgstr_a(expect),
                            wine_dbgstr_an(console_output, console_output_count));
}

#define skip_sequence(a) skip_sequence_(__LINE__,a)
static BOOL skip_sequence_(unsigned int line, const char *expect)
{
    size_t len = strlen(expect);
    DWORD avail;
    BOOL r;

    r = PeekNamedPipe(console_pipe, NULL, 0, NULL, &avail, NULL);
    if (!console_output_count && r && !avail)
    {
        Sleep(50);
        r = PeekNamedPipe(console_pipe, NULL, 0, NULL, &avail, NULL);
    }
    if (r && avail) fetch_console_output_(line);

    if (len > console_output_count || memcmp(console_output, expect, len)) return FALSE;
    console_output_count -= len;
    memmove(console_output, console_output + len, console_output_count);
    return TRUE;
}

#define skip_byte(a) skip_byte_(__LINE__,a)
static BOOL skip_byte_(unsigned int line, char ch)
{
    if (!console_output_count || console_output[0] != ch) return FALSE;
    console_output_count--;
    memmove(console_output, console_output + 1, console_output_count);
    return TRUE;
}

#define expect_hide_cursor() expect_hide_cursor_(__LINE__)
static void expect_hide_cursor_(unsigned int line)
{
    if (!console_output_count) fetch_console_output_(line);
    ok_(__FILE__,line)(skip_sequence_(line, "\x1b[?25l") || broken(skip_sequence_(line, "\x1b[25l")),
                       "expected hide cursor escape\n");
}

#define skip_hide_cursor() skip_hide_cursor_(__LINE__)
static BOOL skip_hide_cursor_(unsigned int line)
{
    if (!console_output_count) fetch_console_output_(line);
    return skip_sequence_(line, "\x1b[25l") || broken(skip_sequence_(line, "\x1b[?25l"));
}

#define expect_erase_line(a) expect_erase_line_(__LINE__,a)
static BOOL expect_erase_line_(unsigned line, unsigned int cnt)
{
    char buf[16];
    if (skip_sequence("\x1b[K")) return FALSE;
    ok(broken(1), "expected erase line\n");
    sprintf(buf, "\x1b[%uX", cnt);
    expect_output_sequence(buf);  /* erase the rest of the line */
    sprintf(buf, "\x1b[%uC", cnt);
    expect_output_sequence(buf);  /* move cursor to the end of the line */
    return TRUE;
}

enum req_type
{
    REQ_CREATE_SCREEN_BUFFER,
    REQ_FILL_CHAR,
    REQ_GET_INPUT,
    REQ_SCROLL,
    REQ_SET_ACTIVE,
    REQ_SET_CURSOR,
    REQ_SET_OUTPUT_MODE,
    REQ_SET_TITLE,
    REQ_WRITE_CHARACTERS,
    REQ_WRITE_CONSOLE,
    REQ_WRITE_OUTPUT,
};

struct pseudoconsole_req
{
    enum req_type type;
    union
    {
        WCHAR string[1];
        COORD coord;
        HANDLE handle;
        DWORD mode;
        struct
        {
            COORD coord;
            unsigned int len;
            WCHAR buf[1];
        } write_characters;
        struct
        {
            COORD size;
            COORD coord;
            SMALL_RECT region;
            CHAR_INFO buf[1];
        } write_output;
        struct
        {
            SMALL_RECT rect;
            COORD dst;
            CHAR_INFO fill;
        } scroll;
        struct
        {
            WCHAR ch;
            DWORD count;
            COORD coord;
        } fill;
    } u;
};

static void child_string_request(enum req_type type, const WCHAR *title)
{
    char buf[4096];
    struct pseudoconsole_req *req = (void *)buf;
    size_t len = lstrlenW(title) + 1;
    DWORD count;
    BOOL ret;

    req->type = type;
    memcpy(req->u.string, title, len * sizeof(WCHAR));
    ret = WriteFile(child_pipe, req, FIELD_OFFSET(struct pseudoconsole_req, u.string[len]),
                    &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static void child_write_characters(const WCHAR *buf, unsigned int x, unsigned int y)
{
    char req_buf[4096];
    struct pseudoconsole_req *req = (void *)req_buf;
    size_t len = lstrlenW(buf);
    DWORD count;
    BOOL ret;

    req->type = REQ_WRITE_CHARACTERS;
    req->u.write_characters.coord.X = x;
    req->u.write_characters.coord.Y = y;
    req->u.write_characters.len = len;
    memcpy(req->u.write_characters.buf, buf, len * sizeof(WCHAR));
    ret = WriteFile(child_pipe, req, FIELD_OFFSET(struct pseudoconsole_req, u.write_characters.buf[len + 1]),
                    &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static void child_set_cursor(const unsigned int x, unsigned int y)
{
    struct pseudoconsole_req req;
    DWORD count;
    BOOL ret;

    req.type = REQ_SET_CURSOR;
    req.u.coord.X = x;
    req.u.coord.Y = y;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static HANDLE child_create_screen_buffer(void)
{
    struct pseudoconsole_req req;
    HANDLE handle;
    DWORD count;
    BOOL ret;

    req.type = REQ_CREATE_SCREEN_BUFFER;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
    ret = ReadFile(child_pipe, &handle, sizeof(handle), &count, NULL);
    ok(ret, "ReadFile failed: %u\n", GetLastError());
    return handle;
}

static void child_set_active(HANDLE handle)
{
    struct pseudoconsole_req req;
    DWORD count;
    BOOL ret;

    req.type = REQ_SET_ACTIVE;
    req.u.handle = handle;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

#define child_write_output(a,b,c,d,e,f,g,h,j,k,l,m,n) child_write_output_(__LINE__,a,b,c,d,e,f,g,h,j,k,l,m,n)
static void child_write_output_(unsigned int line, CHAR_INFO *buf, unsigned int size_x, unsigned int size_y,
                                unsigned int coord_x, unsigned int coord_y, unsigned int left,
                                unsigned int top, unsigned int right, unsigned int bottom, unsigned int out_left,
                                unsigned int out_top, unsigned int out_right, unsigned int out_bottom)
{
    char req_buf[4096];
    struct pseudoconsole_req *req = (void *)req_buf;
    SMALL_RECT region;
    DWORD count;
    BOOL ret;

    req->type = REQ_WRITE_OUTPUT;
    req->u.write_output.size.X = size_x;
    req->u.write_output.size.Y = size_y;
    req->u.write_output.coord.X = coord_x;
    req->u.write_output.coord.Y = coord_y;
    req->u.write_output.region.Left   = left;
    req->u.write_output.region.Top    = top;
    req->u.write_output.region.Right  = right;
    req->u.write_output.region.Bottom = bottom;
    memcpy(req->u.write_output.buf, buf, size_x * size_y * sizeof(*buf));
    ret = WriteFile(child_pipe, req, FIELD_OFFSET(struct pseudoconsole_req, u.write_output.buf[size_x * size_y]), &count, NULL);
    ok_(__FILE__,line)(ret, "WriteFile failed: %u\n", GetLastError());
    ret = ReadFile(child_pipe, &region, sizeof(region), &count, NULL);
    ok_(__FILE__,line)(ret, "WriteFile failed: %u\n", GetLastError());
    ok_(__FILE__,line)(region.Left == out_left, "Left = %u\n", region.Left);
    ok_(__FILE__,line)(region.Top == out_top, "Top = %u\n", region.Top);
    ok_(__FILE__,line)(region.Right == out_right, "Right = %u\n", region.Right);
    ok_(__FILE__,line)(region.Bottom == out_bottom, "Bottom = %u\n", region.Bottom);
}

static void child_scroll(unsigned int src_left, unsigned int src_top, unsigned int src_right,
                         unsigned int src_bottom, unsigned int dst_x, unsigned int dst_y, WCHAR fill)
{
    struct pseudoconsole_req req;
    DWORD count;
    BOOL ret;

    req.type = REQ_SCROLL;
    req.u.scroll.rect.Left   = src_left;
    req.u.scroll.rect.Top    = src_top;
    req.u.scroll.rect.Right  = src_right;
    req.u.scroll.rect.Bottom = src_bottom;
    req.u.scroll.dst.X = dst_x;
    req.u.scroll.dst.Y = dst_y;
    req.u.scroll.fill.Char.UnicodeChar = fill;
    req.u.scroll.fill.Attributes = 0;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static void child_fill_character(WCHAR ch, DWORD count, int x, int y)
{
    struct pseudoconsole_req req;
    BOOL ret;

    req.type = REQ_FILL_CHAR;
    req.u.fill.ch = ch;
    req.u.fill.count = count;
    req.u.fill.coord.X = x;
    req.u.fill.coord.Y = y;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static void child_set_output_mode(DWORD mode)
{
    struct pseudoconsole_req req;
    DWORD count;
    BOOL ret;

    req.type = REQ_SET_OUTPUT_MODE;
    req.u.mode = mode;
    ret = WriteFile(child_pipe, &req, sizeof(req), &count, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());
}

static void expect_input(unsigned int event_type, INPUT_RECORD *record)
{
    struct pseudoconsole_req req = { REQ_GET_INPUT };
    INPUT_RECORD input;
    DWORD read;
    BOOL ret;

    ret = WriteFile(child_pipe, &req, sizeof(req), &read, NULL);
    ok(ret, "WriteFile failed: %u\n", GetLastError());

    ret = ReadFile(child_pipe, &input, sizeof(input), &read, NULL);
    ok(ret, "ReadFile failed: %u\n", GetLastError());

    ok(input.EventType == event_type, "EventType = %u, expected %u\n", input.EventType, event_type);
    if (record) *record = input;
}

static BOOL get_key_input(unsigned int vt, INPUT_RECORD *record)
{
    static INPUT_RECORD prev_record;
    static BOOL have_prev_record;

    if (!have_prev_record)
    {
        expect_input(KEY_EVENT, &prev_record);
        have_prev_record = TRUE;
    }

    if (vt && prev_record.Event.KeyEvent.wVirtualKeyCode != vt) return FALSE;
    *record = prev_record;
    have_prev_record = FALSE;
    return TRUE;
}

#define expect_key_input(a,b,c,d) expect_key_input_(__LINE__,0,a,b,c,d)
static void expect_key_input_(unsigned int line, unsigned int ctx, WCHAR ch, unsigned int vk,
                              BOOL down, unsigned int ctrl_state)
{
    unsigned int vs = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT_RECORD record;

    get_key_input(0, &record);
    ok_(__FILE__,line)(record.Event.KeyEvent.bKeyDown == down, "%x: bKeyDown = %x\n",
                       ctx, record.Event.KeyEvent.bKeyDown);
    ok_(__FILE__,line)(record.Event.KeyEvent.wRepeatCount == 1, "%x: wRepeatCount = %x\n",
                       ctx, record.Event.KeyEvent.wRepeatCount);
    ok_(__FILE__,line)(record.Event.KeyEvent.uChar.UnicodeChar == ch, "%x: UnicodeChar = %x\n",
                       ctx, record.Event.KeyEvent.uChar.UnicodeChar);
    ok_(__FILE__,line)(record.Event.KeyEvent.wVirtualKeyCode == vk,
                       "%x: wVirtualKeyCode = %x, expected %x\n", ctx,
                       record.Event.KeyEvent.wVirtualKeyCode, vk);
    ok_(__FILE__,line)(record.Event.KeyEvent.wVirtualScanCode == vs,
                       "%x: wVirtualScanCode = %x expected %x\n", ctx,
                       record.Event.KeyEvent.wVirtualScanCode, vs);
    ok_(__FILE__,line)(record.Event.KeyEvent.dwControlKeyState == ctrl_state,
                       "%x: dwControlKeyState = %x\n", ctx, record.Event.KeyEvent.dwControlKeyState);
}

#define get_input_key_vt() get_input_key_vt_(__LINE__)
static unsigned int get_input_key_vt_(unsigned int line)
{
    INPUT_RECORD record;

    get_key_input(0, &record);
    ok_(__FILE__,line)(record.Event.KeyEvent.wRepeatCount == 1, "wRepeatCount = %x\n",
                       record.Event.KeyEvent.wRepeatCount);
    return record.Event.KeyEvent.wVirtualKeyCode;
}

#define expect_key_pressed(a,b,c) expect_key_pressed_(__LINE__,0,a,b,c)
#define expect_key_pressed_ctx(a,b,c,d) expect_key_pressed_(__LINE__,a,b,c,d)
static void expect_key_pressed_(unsigned int line, unsigned int ctx, WCHAR ch, unsigned int vk,
                                unsigned int ctrl_state)
{
    if (ctrl_state & SHIFT_PRESSED)
        expect_key_input_(line, ctx, 0, VK_SHIFT, TRUE, SHIFT_PRESSED);
    if (ctrl_state & LEFT_ALT_PRESSED)
        expect_key_input_(line, ctx, 0, VK_MENU, TRUE,
                          LEFT_ALT_PRESSED | (ctrl_state & SHIFT_PRESSED));
    if (ctrl_state & LEFT_CTRL_PRESSED)
        expect_key_input_(line, ctx, 0, VK_CONTROL, TRUE,
                          LEFT_CTRL_PRESSED | (ctrl_state & (SHIFT_PRESSED | LEFT_ALT_PRESSED)));
    expect_key_input_(line, ctx, ch, vk, TRUE, ctrl_state);
    expect_key_input_(line, ctx, ch, vk, FALSE, ctrl_state);
    if (ctrl_state & LEFT_CTRL_PRESSED)
        expect_key_input_(line, ctx, 0, VK_CONTROL, FALSE,
                          ctrl_state & (SHIFT_PRESSED | LEFT_ALT_PRESSED));
    if (ctrl_state & LEFT_ALT_PRESSED)
        expect_key_input_(line, ctx, 0, VK_MENU, FALSE, ctrl_state & SHIFT_PRESSED);
    if (ctrl_state & SHIFT_PRESSED)
        expect_key_input_(line, ctx, 0, VK_SHIFT, FALSE, 0);
}

#define expect_char_key(a) expect_char_key_(__LINE__,a)
static void expect_char_key_(unsigned int line, WCHAR ch)
{
    unsigned int ctrl = 0, vk;
    vk = VkKeyScanW(ch);
    if (vk == ~0) vk = 0;
    if (vk & 0x0100) ctrl |= SHIFT_PRESSED;
    if (vk & 0x0200) ctrl |= LEFT_CTRL_PRESSED;
    vk &= 0xff;
    expect_key_pressed_(line, ch, ch, vk, ctrl);
}

static void test_write_console(void)
{
    child_string_request(REQ_WRITE_CONSOLE, L"abc");
    skip_hide_cursor();
    expect_output_sequence("abc");
    skip_sequence("\x1b[?25h");            /* show cursor */

    child_string_request(REQ_WRITE_CONSOLE, L"\tt");
    skip_hide_cursor();
    if (!skip_sequence("\x1b[3C")) expect_output_sequence("   ");
    expect_output_sequence("t");
    skip_sequence("\x1b[?25h");            /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"x\rr");
    expect_hide_cursor();
    expect_output_sequence("\rr abc   tx");
    if (!skip_sequence("\x1b[9D"))
        expect_output_sequence("\x1b[4;2H"); /* set cursor */
    expect_output_sequence("\x1b[?25h");     /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"yz\r\n");
    skip_hide_cursor();
    expect_output_sequence("yz\r\n");
    skip_sequence("\x1b[?25h");              /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"abc\r\n123\r\ncde\r");
    skip_hide_cursor();
    expect_output_sequence("abc\r\n123\r\ncde\r");
    skip_sequence("\x1b[?25h");              /* show cursor */
    expect_empty_output();

    child_set_cursor(0, 39);
    expect_hide_cursor();
    expect_output_sequence("\x1b[40;1H");    /* set cursor */
    expect_output_sequence("\x1b[?25h");     /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"yz\r\n");
    skip_hide_cursor();
    expect_output_sequence("yz\r");
    if (skip_sequence("\x1b[?25h"))          /* show cursor */
        expect_output_sequence("\x1b[?25l"); /* hide cursor */
    expect_output_sequence("\n");            /* next line */
    if (skip_sequence("\x1b[30X"))           /* erase the line */
    {
        expect_output_sequence("\x1b[30C");  /* move cursor to the end of the line */
        expect_output_sequence("\r");
    }
    skip_sequence("\x1b[?25h");              /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"");
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"ab\n");
    skip_hide_cursor();
    expect_output_sequence("ab");
    if (skip_sequence("\x1b[?25h"))          /* show cursor */
        expect_output_sequence("\x1b[?25l"); /* hide cursor */
    expect_output_sequence("\r\n");          /* next line */
    if (skip_sequence("\x1b[30X"))           /* erase the line */
    {
        expect_output_sequence("\x1b[30C");  /* move cursor to the end of the line */
        expect_output_sequence("\r");
    }
    skip_sequence("\x1b[?25h");              /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 10);
    expect_hide_cursor();
    expect_output_sequence("\x1b[11;29H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");     /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"xy");
    skip_hide_cursor();
    expect_output_sequence("xy");
    if (!skip_sequence("\b")) expect_output_sequence("\r\n");
    skip_sequence("\x1b[?25h");              /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 10);
    fetch_console_output();
    if (!skip_sequence("\b"))
    {
        expect_hide_cursor();
        expect_output_sequence("\x1b[11;29H"); /* set cursor */
        expect_output_sequence("\x1b[?25h");   /* show cursor */
    }
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"abc");
    skip_hide_cursor();
    expect_output_sequence("\r                            ab");
    expect_output_sequence("\r\nc");
    if (expect_erase_line(29))
        expect_output_sequence("\x1b[12;2H"); /* set cursor */
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 39);
    expect_hide_cursor();
    expect_output_sequence("\x1b[40;29H");    /* set cursor */
    expect_output_sequence("\x1b[?25h");      /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"abc");
    skip_hide_cursor();
    expect_output_sequence("ab");
    skip_sequence("\x1b[40;29H");             /* set cursor */
    if (skip_sequence("\x1b[?25h"))           /* show cursor */
        expect_output_sequence("\x1b[?25l");  /* hide cursor */
    else
        skip_sequence("\b");
    expect_output_sequence("\r\nc");
    if (skip_sequence("\x1b[29X"))            /* erase the line */
    {
        expect_output_sequence("\x1b[29C");   /* move cursor to the end of the line */
        expect_output_sequence("\x1b[40;2H"); /* set cursor */
    }
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 39);
    skip_hide_cursor();
    if (!skip_sequence("\x1b[27C"))
        expect_output_sequence("\x1b[40;29H"); /* set cursor */
    skip_sequence("\x1b[?25h");                /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"XY");
    skip_hide_cursor();
    expect_output_sequence("XY");
    skip_sequence("\x1b[40;29H");             /* set cursor */
    if (skip_sequence("\x1b[?25h"))           /* show cursor */
        expect_output_sequence("\x1b[?25l");  /* hide cursor */
    if (!skip_sequence("\b"))
    {
        expect_output_sequence("\r\n");
        expect_output_sequence("\x1b[30X");   /* erase the line */
        expect_output_sequence("\x1b[30C");   /* move cursor to the end of the line */
        expect_output_sequence("\r");         /* set cursor */
    }
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_string_request(REQ_WRITE_CONSOLE, L"\n");
    skip_hide_cursor();
    if (!skip_sequence("\r\n"))
    {
        expect_output_sequence("\n");
        expect_output_sequence("\x1b[30X");   /* erase the line */
        expect_output_sequence("\x1b[30C");   /* move cursor to the end of the line */
        expect_output_sequence("\r");         /* set cursor */
    }
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_set_output_mode(ENABLE_PROCESSED_OUTPUT);

    child_set_cursor(28, 11);
    expect_hide_cursor();
    expect_output_sequence("\x1b[12;29H");    /* set cursor */
    skip_sequence("\x1b[?25h");               /* show cursor */

    child_string_request(REQ_WRITE_CONSOLE, L"xyz1234");
    skip_hide_cursor();
    expect_output_sequence("43\b");
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 11);
    skip_hide_cursor();
    expect_output_sequence("\b");             /* backspace */
    skip_sequence("\x1b[?25h");               /* show cursor */

    child_string_request(REQ_WRITE_CONSOLE, L"xyz123");
    expect_hide_cursor();
    expect_output_sequence("23");
    if (!skip_sequence("\x1b[2D"))
        expect_output_sequence("\x1b[12;29H");/* set cursor */
    expect_output_sequence("\x1b[?25h");      /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 11);
    child_string_request(REQ_WRITE_CONSOLE, L"abcdef\n\r123456789012345678901234567890xyz");
    expect_hide_cursor();
    if (skip_sequence("\x1b[?25h")) expect_hide_cursor();
    expect_output_sequence("\r                            ef\r\n");
    expect_output_sequence("xyz456789012345678901234567890");
    if (!skip_sequence("\x1b[27D"))
        expect_output_sequence("\x1b[13;4H"); /* set cursor */
    expect_output_sequence("\x1b[?25h");      /* show cursor */
    expect_empty_output();

    child_set_cursor(28, 11);
    expect_hide_cursor();
    expect_output_sequence("\x1b[12;29H");    /* set cursor */
    expect_output_sequence("\x1b[?25h");      /* show cursor */

    child_string_request(REQ_WRITE_CONSOLE, L"AB\r\n");
    skip_hide_cursor();
    expect_output_sequence("AB\r\n");
    skip_sequence("\x1b[?25h");               /* show cursor */
    expect_empty_output();

    child_set_output_mode(ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
}

static void test_tty_output(void)
{
    CHAR_INFO char_info_buf[2048], char_info;
    HANDLE sb, sb2;
    unsigned int i;

    /* simple write chars */
    child_write_characters(L"child", 3, 4);
    expect_hide_cursor();
    expect_output_sequence("\x1b[5;4H");   /* set cursor */
    expect_output_sequence("child");
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* wrapped write chars */
    child_write_characters(L"bound", 28, 6);
    expect_hide_cursor();
    expect_output_sequence("\x1b[7;1H");   /* set cursor */
    expect_output_sequence("                            bo\r\nund");
    expect_erase_line(27);
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* fill line 4 with a few simple writes */
    child_write_characters(L"xxx", 13, 4);
    expect_hide_cursor();
    expect_output_sequence("\x1b[5;14H");  /* set cursor */
    expect_output_sequence("xxx");
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* write one char at the end of row */
    child_write_characters(L"y", 29, 4);
    expect_hide_cursor();
    expect_output_sequence("\x1b[5;30H");  /* set cursor */
    expect_output_sequence("y");
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* wrapped write chars */
    child_write_characters(L"zz", 29, 4);
    expect_hide_cursor();
    expect_output_sequence("\x1b[5;1H");   /* set cursor */
    expect_output_sequence("   child     xxx             z");
    expect_output_sequence("\r\nz");
    expect_erase_line(29);
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* trailing spaces */
    child_write_characters(L"child        ", 3, 4);
    expect_hide_cursor();
    expect_output_sequence("\x1b[5;4H");   /* set cursor */
    expect_output_sequence("child        ");
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_set_cursor(2, 3);
    expect_hide_cursor();
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_string_request(REQ_SET_TITLE, L"new title");
    fetch_console_output();
    skip_sequence("\x1b[?25l");            /* hide cursor */
    expect_output_sequence("\x1b]0;new title\x07"); /* set title */
    skip_sequence("\x1b[?25h");            /* show cursor */
    expect_empty_output();

    for (i = 0; i < ARRAY_SIZE(char_info_buf); i++)
    {
        char_info_buf[i].Char.UnicodeChar = '0' + i % 10;
        char_info_buf[i].Attributes = 0;
    }

    child_write_output(char_info_buf, /* size */ 7, 8, /* coord */ 1, 2,
                       /* region */ 3, 7, 5, 9, /* out region */ 3, 7, 5, 9);
    expect_hide_cursor();
    expect_output_sequence("\x1b[30m");    /* foreground black */
    expect_output_sequence("\x1b[8;4H");   /* set cursor */
    expect_output_sequence("567");
    expect_output_sequence("\x1b[9;4H");   /* set cursor */
    expect_output_sequence("234");
    expect_output_sequence("\x1b[10;4H");  /* set cursor */
    expect_output_sequence("901");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_output(char_info_buf, /* size */ 2, 3, /* coord */ 1, 2,
                       /* region */ 3, 8, 15, 19, /* out region */ 3, 8, 3, 8);
    expect_hide_cursor();
    if (skip_sequence("\x1b[m"))           /* default attr */
        expect_output_sequence("\x1b[30m");/* foreground black */
    expect_output_sequence("\x1b[9;4H");   /* set cursor */
    expect_output_sequence("5");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_output(char_info_buf, /* size */ 3, 4, /* coord */ 1, 2,
                       /* region */ 3, 8, 15, 19, /* out region */ 3, 8, 4, 9);
    expect_hide_cursor();
    if (skip_sequence("\x1b[m"))           /* default attr */
        expect_output_sequence("\x1b[30m");/* foreground black */
    expect_output_sequence("\x1b[9;4H");   /* set cursor */
    expect_output_sequence("78");
    expect_output_sequence("\x1b[10;4H");  /* set cursor */
    expect_output_sequence("01");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_output(char_info_buf, /* size */ 7, 8, /* coord */ 2, 3,
                       /* region */ 28, 38, 31, 60, /* out region */ 28, 38, 29, 39);
    expect_hide_cursor();
    if (skip_sequence("\x1b[m"))           /* default attr */
        expect_output_sequence("\x1b[30m");/* foreground black */
    expect_output_sequence("\x1b[39;29H"); /* set cursor */
    expect_output_sequence("34");
    expect_output_sequence("\x1b[40;29H"); /* set cursor */
    expect_output_sequence("01");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_output(char_info_buf, /* size */ 7, 8, /* coord */ 1, 2,
                       /* region */ 0, 7, 5, 9, /* out region */ 0, 7, 5, 9);
    expect_hide_cursor();
    if (skip_sequence("\x1b[m"))           /* default attr */
        expect_output_sequence("\x1b[30m");/* foreground black */
    expect_output_sequence("\x1b[8;1H");   /* set cursor */
    expect_output_sequence("567890\r\n");
    expect_output_sequence("234567\r\n");
    expect_output_sequence("901234");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_scroll(/* scroll rect */ 0, 7, 2, 8, /* destination */ 2, 8, /* fill */ 'x');
    expect_hide_cursor();
    if (skip_sequence("\x1b[m"))           /* default attr */
        expect_output_sequence("\x1b[30m");/* foreground black */
    expect_output_sequence("\x1b[8;1H");   /* set cursor */
    expect_output_sequence("xxx89\r\n");
    expect_output_sequence("xx567\r\n");
    expect_output_sequence("90234");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_characters(L"xxx", 3, 10);
    expect_hide_cursor();
    expect_output_sequence("\x1b[m");      /* default attributes */
    expect_output_sequence("\x1b[11;4H");  /* set cursor */
    expect_output_sequence("xxx");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    /* test attributes */
    for (i = 0; i < 0x100 - 0xff; i++)
    {
        unsigned int expect;
        char expect_buf[16];
        char_info.Char.UnicodeChar = 'a';
        char_info.Attributes = i;
        child_write_output(&char_info, /* size */ 1, 1, /* coord */ 0, 0,
                           /* region */ 12, 3, 12, 3, /* out region */ 12, 3, 12, 3);
        expect_hide_cursor();
        if (i != 0x190 && i && ((i & 0xff) != 8)) expect_output_sequence_ctx(i, "\x1b[m");
        if ((i & 0x0f) != 7)
        {
            expect = 30;
            if (i & FOREGROUND_BLUE)  expect += 4;
            if (i & FOREGROUND_GREEN) expect += 2;
            if (i & FOREGROUND_RED)   expect += 1;
            if (i & FOREGROUND_INTENSITY) expect += 60;
            sprintf(expect_buf, "\x1b[%um", expect);
            expect_output_sequence_ctx(i, expect_buf);
        }
        if (i & 0xf0)
        {
            expect = 40;
            if (i & BACKGROUND_BLUE)  expect += 4;
            if (i & BACKGROUND_GREEN) expect += 2;
            if (i & BACKGROUND_RED)   expect += 1;
            if (i & BACKGROUND_INTENSITY) expect += 60;
            sprintf(expect_buf, "\x1b[%um", expect);
            expect_output_sequence_ctx(i, expect_buf);
        }
        if (!skip_sequence("\x1b[10C"))
            expect_output_sequence_ctx(i, "\x1b[4;13H"); /* set cursor */
        expect_output_sequence("a");
        if (!skip_sequence("\x1b[11D"))
            expect_output_sequence("\x1b[4;3H"); /* set cursor */
        expect_output_sequence("\x1b[?25h");     /* show cursor */
        expect_empty_output();
    }

    char_info_buf[0].Attributes = FOREGROUND_GREEN;
    char_info_buf[1].Attributes = FOREGROUND_GREEN | BACKGROUND_RED;
    char_info_buf[2].Attributes = BACKGROUND_RED;
    child_write_output(char_info_buf, /* size */ 7, 8, /* coord */ 0, 0,
                       /* region */ 7, 0, 9, 0, /* out region */ 7, 0, 9, 0);
    expect_hide_cursor();
    skip_sequence("\x1b[m");               /* default attr */
    expect_output_sequence("\x1b[32m");    /* foreground black */
    expect_output_sequence("\x1b[1;8H");   /* set cursor */
    expect_output_sequence("0");
    expect_output_sequence("\x1b[41m");    /* backgorund red */
    expect_output_sequence("1");
    expect_output_sequence("\x1b[30m");    /* foreground black */
    expect_output_sequence("2");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_fill_character('i', 5, 15, 16);
    expect_hide_cursor();
    expect_output_sequence("\x1b[m");      /* default attributes */
    expect_output_sequence("\x1b[17;16H"); /* set cursor */
    expect_output_sequence("iiiii");
    expect_output_sequence("\x1b[4;3H");   /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    test_write_console();

    sb = child_create_screen_buffer();
    child_set_active(sb);
    expect_hide_cursor();
    expect_output_sequence("\x1b[H");      /* set cursor */
    for (i = 0; i < 40; i++)
    {
        expect_erase_line(30);
        if (i != 39) expect_output_sequence("\r\n");
    }
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_write_characters(L"new sb", 0, 0);
    skip_hide_cursor();
    expect_output_sequence("new sb");
    ok(skip_sequence("\x1b[H") || skip_sequence("\r"), "expected set cursor\n");
    skip_sequence("\x1b[?25h");            /* show cursor */
    expect_empty_output();

    sb2 = child_create_screen_buffer();
    child_set_active(sb2);
    expect_hide_cursor();
    for (i = 0; i < 40; i++)
    {
        expect_erase_line(30);
        if (i != 39) expect_output_sequence("\r\n");
    }
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();

    child_set_active(sb);
    expect_hide_cursor();
    expect_output_sequence("new sb");
    expect_erase_line(24);
    expect_output_sequence("\r\n");
    for (i = 1; i < 40; i++)
    {
        expect_erase_line(30);
        if (i != 39) expect_output_sequence("\r\n");
    }
    expect_output_sequence("\x1b[H");      /* set cursor */
    expect_output_sequence("\x1b[?25h");   /* show cursor */
    expect_empty_output();
}

static void write_console_pipe(const char *buf)
{
    DWORD written;
    BOOL res;
    res = WriteFile(console_pipe, buf, strlen(buf), &written, NULL);
    ok(res, "WriteFile failed: %u\n", GetLastError());
}

static void test_tty_input(void)
{
    INPUT_RECORD ir;
    unsigned int i;
    char buf[8];

    static const struct
    {
        const char *str;
        WCHAR ch;
        unsigned int vk;
        unsigned int ctrl;
    } escape_test[] = {
        { "\x1b[A",          0,      VK_UP,       0 },
        { "\x1b[B",          0,      VK_DOWN,     0 },
        { "\x1b[C",          0,      VK_RIGHT,    0 },
        { "\x1b[D",          0,      VK_LEFT,     0 },
        { "\x1b[H",          0,      VK_HOME,     0 },
        { "\x1b[F",          0,      VK_END,      0 },
        { "\x1b[2~",         0,      VK_INSERT,   0 },
        { "\x1b[3~",         0,      VK_DELETE,   0 },
        { "\x1b[5~",         0,      VK_PRIOR,    0 },
        { "\x1b[6~",         0,      VK_NEXT,     0 },
        { "\x1b[15~",        0,      VK_F5,       0 },
        { "\x1b[17~",        0,      VK_F6,       0 },
        { "\x1b[18~",        0,      VK_F7,       0 },
        { "\x1b[19~",        0,      VK_F8,       0 },
        { "\x1b[20~",        0,      VK_F9,       0 },
        { "\x1b[21~",        0,      VK_F10,      0 },
        /* 0x10 */
        { "\x1b[23~",        0,      VK_F11,      0 },
        { "\x1b[24~",        0,      VK_F12,      0 },
        { "\x1bOP",          0,      VK_F1,       0 },
        { "\x1bOQ",          0,      VK_F2,       0 },
        { "\x1bOR",          0,      VK_F3,       0 },
        { "\x1bOS",          0,      VK_F4,       0 },
        { "\x1b[1;1A",       0,      VK_UP,       0 },
        { "\x1b[1;2A",       0,      VK_UP,       SHIFT_PRESSED },
        { "\x1b[1;3A",       0,      VK_UP,       LEFT_ALT_PRESSED },
        { "\x1b[1;4A",       0,      VK_UP,       SHIFT_PRESSED | LEFT_ALT_PRESSED },
        { "\x1b[1;5A",       0,      VK_UP,       LEFT_CTRL_PRESSED },
        { "\x1b[1;6A",       0,      VK_UP,       SHIFT_PRESSED | LEFT_CTRL_PRESSED },
        { "\x1b[1;7A",       0,      VK_UP,       LEFT_ALT_PRESSED  | LEFT_CTRL_PRESSED },
        { "\x1b[1;8A",       0,      VK_UP,       SHIFT_PRESSED | LEFT_ALT_PRESSED  | LEFT_CTRL_PRESSED },
        { "\x1b[1;9A",       0,      VK_UP,       0 },
        { "\x1b[1;10A",      0,      VK_UP,       SHIFT_PRESSED },
        /* 0x20 */
        { "\x1b[1;11A",      0,      VK_UP,       LEFT_ALT_PRESSED },
        { "\x1b[1;12A",      0,      VK_UP,       SHIFT_PRESSED | LEFT_ALT_PRESSED },
        { "\x1b[1;13A",      0,      VK_UP,       LEFT_CTRL_PRESSED },
        { "\x1b[1;14A",      0,      VK_UP,       SHIFT_PRESSED | LEFT_CTRL_PRESSED },
        { "\x1b[1;15A",      0,      VK_UP,       LEFT_ALT_PRESSED  | LEFT_CTRL_PRESSED },
        { "\x1b[1;16A",      0,      VK_UP,       SHIFT_PRESSED | LEFT_ALT_PRESSED  | LEFT_CTRL_PRESSED },
        { "\x1b[1;2P",       0,      VK_F1,       SHIFT_PRESSED },
        { "\x1b[2;3~",       0,      VK_INSERT,   LEFT_ALT_PRESSED },
        { "\x1b[2;3;5;6~",   0,      VK_INSERT,   0 },
        { "\x1b[6;2;3;5;1~", 0,      VK_NEXT,     0 },
        { "\xe4\xb8\x80",    0x4e00, 0,           0 },
        { "\x1b\x1b",        0x1b,   VK_ESCAPE,   LEFT_ALT_PRESSED },
        { "\x1b""1",         '1',    '1',         LEFT_ALT_PRESSED },
        { "\x1b""x",         'x',    'X',         LEFT_ALT_PRESSED },
        { "\x1b""[",         '[',    VK_OEM_4,    LEFT_ALT_PRESSED },
        { "\x7f",            '\b',   VK_BACK,     0 },
    };

    write_console_pipe("x");
    if (!get_input_key_vt())
    {
        skip("Skipping tests on settings that don't have VT mapping for 'x'\n");
        get_input_key_vt();
        return;
    }
    get_input_key_vt();

    write_console_pipe("aBCd");
    expect_char_key('a');
    expect_char_key('B');
    expect_char_key('C');
    expect_char_key('d');

    for (i = 1; i < 0x7f; i++)
    {
        if (i == 3 || i == '\n' || i == 0x1b || i == 0x1f) continue;
        buf[0] = i;
        buf[1] = 0;
        write_console_pipe(buf);
        if (i == 8)
            expect_key_pressed('\b', 'H', LEFT_CTRL_PRESSED);
        else if (i == 0x7f)
            expect_char_key(8);
        else
            expect_char_key(i);
    }

    write_console_pipe("\r\n");
    expect_key_pressed('\r', VK_RETURN, 0);
    expect_key_pressed('\n', VK_RETURN, LEFT_CTRL_PRESSED);

    write_console_pipe("\xc4\x85");
    if (get_key_input(VK_MENU, &ir))
    {
        expect_key_input(0x105, 'A', TRUE, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
        expect_key_input(0x105, 'A', FALSE, LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED);
        expect_key_input(0, VK_MENU, FALSE, ENHANCED_KEY);
    }
    else
    {
        expect_key_input(0x105, 0, TRUE, 0);
        expect_key_input(0x105, 0, FALSE, 0);
    }

    for (i = 0; i < ARRAY_SIZE(escape_test); i++)
    {
        write_console_pipe(escape_test[i].str);
        expect_key_pressed_ctx(i, escape_test[i].ch, escape_test[i].vk, escape_test[i].ctrl);
    }

    for (i = 0x80; i < 0x100; i += 11)
    {
        buf[0] = i;
        buf[1] = 0;
        write_console_pipe(buf);
        expect_empty_output();
    }
}

static void child_process(HANDLE pipe)
{
    HANDLE output, input;
    DWORD size, count;
    char buf[4096];
    BOOL ret;

    output = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0);
    ok(output != INVALID_HANDLE_VALUE, "could not open console output\n");

    input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0);
    ok(output != INVALID_HANDLE_VALUE, "could not open console output\n");

    while(ReadFile(pipe, buf, sizeof(buf), &size, NULL))
    {
        const struct pseudoconsole_req *req = (void *)buf;
        switch (req->type)
        {
        case REQ_CREATE_SCREEN_BUFFER:
            {
                HANDLE handle;
                DWORD count;
                SetLastError(0xdeadbeef);
                handle = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                                   CONSOLE_TEXTMODE_BUFFER, NULL);
                ok(handle != INVALID_HANDLE_VALUE, "CreateConsoleScreenBuffer failed: %u\n", GetLastError());
                ret = WriteFile(pipe, &handle, sizeof(handle), &count, NULL);
                ok(ret, "WriteFile failed: %u\n", GetLastError());
                break;
            }

        case REQ_GET_INPUT:
            {
                INPUT_RECORD record;
                ret = ReadConsoleInputW(input, &record, 1, &count);
                ok(ret, "ReadConsoleInputW failed: %u\n", GetLastError());
                ok(count == 1, "count = %u\n", count);
                ret = WriteFile(pipe, &record, sizeof(record), &count, NULL);
                ok(ret, "WriteFile failed: %u\n", GetLastError());
                break;
            }

        case REQ_SCROLL:
            ret = ScrollConsoleScreenBufferW(output, &req->u.scroll.rect, NULL, req->u.scroll.dst, &req->u.scroll.fill);
            ok(ret, "ScrollConsoleScreenBuffer failed: %u\n", GetLastError());
            break;

        case REQ_FILL_CHAR:
            ret = FillConsoleOutputCharacterW(output, req->u.fill.ch, req->u.fill.count, req->u.fill.coord, &count);
            ok(ret, "FillConsoleOutputCharacter failed: %u\n", GetLastError());
            ok(count == req->u.fill.count, "count = %u, expected %u\n", count, req->u.fill.count);
            break;

        case REQ_SET_ACTIVE:
            output = req->u.handle;
            ret = SetConsoleActiveScreenBuffer(output);
            ok(ret, "SetConsoleActiveScreenBuffer failed: %u\n", GetLastError());
            break;

        case REQ_SET_CURSOR:
            ret = SetConsoleCursorPosition(output, req->u.coord);
            ok(ret, "SetConsoleCursorPosition failed: %u\n", GetLastError());
            break;

        case REQ_SET_OUTPUT_MODE:
            ret = SetConsoleMode(output, req->u.mode);
            ok(ret, "SetConsoleMode failed: %u\n", GetLastError());
            break;

        case REQ_SET_TITLE:
            ret = SetConsoleTitleW(req->u.string);
            ok(ret, "SetConsoleTitleW failed: %u\n", GetLastError());
            break;

        case REQ_WRITE_CHARACTERS:
            ret = WriteConsoleOutputCharacterW(output, req->u.write_characters.buf,
                                               req->u.write_characters.len,
                                               req->u.write_characters.coord, &count);
            ok(ret, "WriteConsoleOutputCharacterW failed: %u\n", GetLastError());
            break;

        case REQ_WRITE_CONSOLE:
            ret = WriteConsoleW(output, req->u.string, lstrlenW(req->u.string), NULL, NULL);
            ok(ret, "SetConsoleTitleW failed: %u\n", GetLastError());
            break;

        case REQ_WRITE_OUTPUT:
            {
                SMALL_RECT region = req->u.write_output.region;
                ret = WriteConsoleOutputW(output, req->u.write_output.buf, req->u.write_output.size, req->u.write_output.coord, &region);
                ok(ret, "WriteConsoleOutput failed: %u\n", GetLastError());
                ret = WriteFile(pipe, &region, sizeof(region), &count, NULL);
                ok(ret, "WriteFile failed: %u\n", GetLastError());
                break;
            }

        default:
            ok(0, "unexpected request type %u\n", req->type);
        };
    }
    ok(GetLastError() == ERROR_BROKEN_PIPE, "ReadFile failed: %u\n", GetLastError());
    CloseHandle(output);
    CloseHandle(input);
}

static HANDLE run_child(HANDLE console, HANDLE pipe)
{
    STARTUPINFOEXA startup = {{ sizeof(startup) }};
    char **argv, cmdline[MAX_PATH];
    PROCESS_INFORMATION info;
    SIZE_T size;
    BOOL ret;

    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, size);
    InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size);
    UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                              sizeof(console), NULL, NULL);

    winetest_get_mainargs(&argv);
    sprintf(cmdline, "\"%s\" %s child %p", argv[0], argv[1], pipe);
    ret = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                         &startup.StartupInfo, &info);
    ok(ret, "CreateProcessW failed: %u\n", GetLastError());

    CloseHandle(info.hThread);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    return info.hProcess;
}

static HPCON create_pseudo_console(HANDLE *console_pipe_end, HANDLE *child_process)
{
    SECURITY_ATTRIBUTES sec_attr = { sizeof(sec_attr), NULL, TRUE };
    HANDLE child_pipe_end;
    COORD size = { 30, 40 };
    DWORD read_mode;
    HPCON console;
    HRESULT hres;
    BOOL r;

    console_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\pseudoconsoleconn", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_WAIT | PIPE_TYPE_BYTE, 1, 4096, 4096, NMPWAIT_USE_DEFAULT_WAIT, NULL);
    ok(console_pipe != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed: %u\n", GetLastError());

    *console_pipe_end = CreateFileW(L"\\\\.\\pipe\\pseudoconsoleconn", GENERIC_READ | GENERIC_WRITE,
                                    0, &sec_attr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    ok(*console_pipe_end != INVALID_HANDLE_VALUE, "CreateFile failed: %u\n", GetLastError());

    child_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\pseudoconsoleserver", PIPE_ACCESS_DUPLEX,
                                  PIPE_WAIT | PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, 1, 5000, 6000,
                                  NMPWAIT_USE_DEFAULT_WAIT, NULL);
    ok(child_pipe != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed: %u\n", GetLastError());

    child_pipe_end = CreateFileW(L"\\\\.\\pipe\\pseudoconsoleserver", GENERIC_READ | GENERIC_WRITE, 0,
                                 &sec_attr, OPEN_EXISTING, 0, NULL);
    ok(child_pipe_end != INVALID_HANDLE_VALUE, "CreateFile failed: %u\n", GetLastError());

    read_mode = PIPE_READMODE_MESSAGE;
    r = SetNamedPipeHandleState(child_pipe_end, &read_mode, NULL, NULL);
    ok(r, "SetNamedPipeHandleState failed: %u\n", GetLastError());

    hres = pCreatePseudoConsole(size, *console_pipe_end, *console_pipe_end, 0, &console);
    ok(hres == S_OK, "CreatePseudoConsole failed: %08x\n", hres);

    *child_process = run_child(console, child_pipe_end);
    CloseHandle(child_pipe_end);
    return console;
}

static void test_pseudoconsole(void)
{
    HANDLE console_pipe_end, child_process;
    BOOL broken_version;
    HPCON console;

    console = create_pseudo_console(&console_pipe_end, &child_process);

    child_string_request(REQ_SET_TITLE, L"test title");
    expect_output_sequence("\x1b[2J");   /* erase display */
    skip_hide_cursor();
    expect_output_sequence("\x1b[m");    /* default attributes */
    expect_output_sequence("\x1b[H");    /* set cursor */
    skip_sequence("\x1b[H");             /* some windows versions emit it twice */
    expect_output_sequence("\x1b]0;test title"); /* set title */
    broken_version = skip_byte(0);       /* some win versions emit nullbyte */
    expect_output_sequence("\x07");
    skip_sequence("\x1b[?25h");          /* show cursor */
    expect_empty_output();

    if (!broken_version)
    {
        test_tty_output();
        test_tty_input();
    }
    else win_skip("Skipping tty output tests on broken Windows version\n");

    pClosePseudoConsole(console);
    CloseHandle(console_pipe_end);
    CloseHandle(console_pipe);
}

START_TEST(tty)
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc > 3)
    {
        HANDLE pipe;
        sscanf(argv[3], "%p", &pipe);
        child_process(pipe);
        return;
    }

    pCreatePseudoConsole = (void *)GetProcAddress(kernel32, "CreatePseudoConsole");
    pClosePseudoConsole  = (void *)GetProcAddress(kernel32, "ClosePseudoConsole");
    if (!pCreatePseudoConsole)
    {
        win_skip("CreatePseudoConsole is not available\n");
        return;
    }

    test_pseudoconsole();
}
