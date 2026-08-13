// pico-speccy — the on-SD sorted directory index shared by the file browsers.
//
// Shared by the fullscreen browser (src/ui/UiBrowser.cpp) and the remote/web
// browsers in OSDFile.cpp: they use the SAME .idx files under /tmp, and the cache
// invalidation (content CRC + SORT_VERSION) stays in one place.
//
// The index is a flat file of fixed-size records (FF_LFN_BUF+1 bytes, one
// filename each, DIR_MARKER prefix for directories) living at
// /tmp/.<mangled-dir>.idx — always on SD, so a removed USB stick can't break it.
// Everything is inline and file-backed: zero heap, which is the point on a
// ~100 KB free-heap system with directories of thousands of entries.

#pragma once

#include <string>
#include <algorithm>
#include "ff.h"
#include "fabutils.h"   // DIR_MARKER, fabgl::VirtualKey

// Sort version: bump to invalidate cached .idx files when sort order changes
#define SORT_VERSION 1

// Defined in ESPectrum.cpp: last key seen by the keyboard ISR. The qsort below
// polls it so a long sort of a huge directory can be aborted with F1.
fabgl::VirtualKey get_last_key_pressed(void);

inline size_t sf_name_crc(const std::string& s) {
    size_t res = 0;
    for (size_t j = 0; j < s.size(); ++j) {
        res += s[j];
    }
    return res;
}

class sorted_files {
    static const size_t rec_size = FF_LFN_BUF + 1;
    std::string folder;
    std::string idx_file;
    size_t sz = 0;
    FIL* storage_file = 0;
    bool open = false;
    inline void calc_sz() {
        sz = 0;
        storage_file = fopen2(idx_file.c_str(), FA_READ);
        if (storage_file) {
            UINT br;
            char buf[rec_size];
            while ( f_read(storage_file, buf, rec_size, &br) == FR_OK && br == rec_size ) {
                ++sz;
            }
            fclose2(storage_file);
        }
        storage_file = fopen2(idx_file.c_str(), FA_READ | FA_WRITE);
        if (storage_file) open = true;
    }
public:
    inline sorted_files() { }
    inline void close(void) { if (open && storage_file) fclose2(storage_file); open = false; }
    inline ~sorted_files() { close(); }
    inline size_t size(void) { return sz; }
    inline bool is_open(void) { return open; }
    inline void unlink(void) {
        close();
        f_unlink(idx_file.c_str());
        storage_file = fopen2(idx_file.c_str(), FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
        if (storage_file) open = true;
        sz = 0;
    }
    inline void init(const std::string& folder) {
        close();
        this->folder = folder;
        std::string s = folder;
        std::replace( s.begin(), s.end(), '/', '_');
        std::replace( s.begin(), s.end(), ':', '_');  // "USB:/..." — ':' is invalid in FAT names
        idx_file = "/tmp/." + s + ".idx";
        calc_sz();
    }
    inline void put(size_t i, const std::string& s) {
        if (!open || !storage_file) return;    // create failed: degrade, don't fault
        f_lseek(storage_file, rec_size * i);
        UINT bw;
        char buf[rec_size] = { 0 };
        strncpy(buf, s.c_str(), rec_size - 1);
        f_write(storage_file, buf, rec_size, &bw);
    }
    inline void push(const std::string& s) {
        put(sz++, s);
    }
    inline size_t crc(void) {
        size_t res = SORT_VERSION;
        for (size_t i = 0; i < sz; ++i) {
            res += sf_name_crc(get(i));
        }
        return res;
    }
    inline std::string get(size_t i) {
        if (!open || !storage_file) return std::string();
        // buf MUST be zeroed and the read length MUST be checked: a short read (the
        // record is missing because a put() write failed, or the index file was
        // truncated/cross-linked under us) used to return the raw stack frame, so the
        // browser drew binary junk — residue of whatever strings that stack last held
        // — instead of an empty row, hiding the real failure. hw 2026-08-13.
        UINT br = 0;
        char buf[rec_size] = { 0 };
        if (f_lseek(storage_file, rec_size * i) != FR_OK) return std::string();
        if (f_read(storage_file, buf, rec_size, &br) != FR_OK || br < rec_size)
            return std::string();
        buf[rec_size - 1] = '\0';
        return (buf);
    }
    inline std::string operator[](size_t i) {
        return get(i);
    }
    inline int cmp(const std::string& s1, const std::string& s2) {
        // Case-insensitive compare; DIR_MARKER (0x01) stays lowest so dirs sort first
        size_t len = s1.size() < s2.size() ? s1.size() : s2.size();
        for (size_t i = 0; i < len; i++) {
            int c1 = (uint8_t)s1[i] == DIR_MARKER ? s1[i] : toupper((uint8_t)s1[i]);
            int c2 = (uint8_t)s2[i] == DIR_MARKER ? s2[i] : toupper((uint8_t)s2[i]);
            if (c1 != c2) return c1 - c2;
        }
        return (int)s1.size() - (int)s2.size();
    }
    inline int cmp(size_t i1, size_t i2) {
        return cmp(get(i1), get(i2));
    }
    inline void swap(size_t i1, size_t i2) {
        std::string s1 = get(i1);
        std::string s2 = get(i2);
        put(i1, s2);
        put(i2, s1);
    }
    inline void vecswap(size_t i1, size_t i2, size_t num) {
        for (size_t i = 0; i < num; ++i) {
            swap(i1 + i, i2 + i);
        }
    }
    inline size_t med3(size_t a, size_t b, size_t c) {
	    return cmp(a, b) < 0 ? (cmp(b, c) < 0 ? b : (cmp(a, c) < 0 ? c : a )) : (cmp(b, c) > 0 ? b : (cmp(a, c) < 0 ? a : c ));
    }
    inline void sort(void) {
        qsort(0, sz);
    }
    void qsort(size_t ai, size_t n) {
        if (!n) return;
        size_t pn, pm, pl, d, pa, pb, pc, pd = 0;
        int r;
    loop:
        fabgl::VirtualKey lkp = get_last_key_pressed();
        if (lkp == fabgl::VirtualKey::VK_F1) return;
        size_t swap_cnt = 0;
        if (n < 7) {
            for (pm = ai + 1; pm < ai + n; ++pm) {
                for (pl = pm; pl > ai && cmp(pl - 1, pl) > 0; --pl) {
                    swap(pl, pl - 1);
                }
            }
        }
        pm = ai + (n / 2);
	    if (n > 7) {
		    pl = ai;
		    pn = ai + (n - 1);
		    if (n > 40) {
			    d = (n / 8);
			    pl = med3(pl, pl + d, pl + 2 * d);
			    pm = med3(pm - d, pm, pm + d);
    			pn = med3(pn - 2 * d, pn - d, pn);
	    	}
		    pm = med3(pl, pm, pn);
	    }
	    swap(ai, pm);
	    pa = pb = ai + 1;
        pc = pd = ai + (n - 1);
	    for (;;) {
		    while (pb <= pc && (r = cmp(pb, ai)) <= 0) {
                fabgl::VirtualKey lkp = get_last_key_pressed();
                if (lkp == fabgl::VirtualKey::VK_F1) return;
			    if (r == 0) {
				    swap_cnt = 1;
				    swap(pa, pb);
				    ++pa;
                }
			    ++pb;
		    }
		    while (pb <= pc && (r = cmp(pc, ai)) >= 0) {
                fabgl::VirtualKey lkp = get_last_key_pressed();
                if (lkp == fabgl::VirtualKey::VK_F1) return;
			    if (r == 0) {
				    swap_cnt = 1;
				    swap(pc, pd);
				    --pd;
			    }
			    --pc;
		    }
		    if (pb > pc)
			    break;
		    swap(pb, pc);
		    swap_cnt = 1;
		    ++pb;
		    --pc;
	    }
	    if (swap_cnt == 0) {  // Switch to insertion sort
		    for (pm = ai + 1; pm < ai + n; ++pm)
			    for (pl = pm; pl > ai && cmp(pl - 1, pl) > 0; --pl) {
                    fabgl::VirtualKey lkp = get_last_key_pressed();
                    if (lkp == fabgl::VirtualKey::VK_F1) return;
				    swap(pl, pl - 1);
                }
		    return;
        }
	    pn = ai + n;
	    r = std::min(pa - ai, pb - pa);
	    vecswap(ai, pb - r, r);
	    r = std::min(pd - pc, pn - pd - 1);
	    vecswap(pb, pn - r, r);
	    if ((r = pb - pa) > 1)
		qsort(ai, r);
	    if ((r = pd - pc) > 1) {
		    // Iterate rather than recurse to save stack space
		    ai = pn - r;
		    n = r;
		    goto loop;
	    }
    }
};
