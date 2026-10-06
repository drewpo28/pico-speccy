#ifndef FileInfo_h
#define FileInfo_h

#include <string>
using namespace std;

class FileInfo {
public:
    // Show file info in OSD dialog (dispatches by extension)
    static void viewInfo(const string& path);
    // The machine a snapshot was made on, from its first bytes (<= 87 needed) and
    // its total size; "" for a format this does not know.
    static string snapshotMachine(const char* ext, const unsigned char* hdr, unsigned n, unsigned total);
};

#endif
