#include <sys/stat.h>
#include <cstring>
#include "wfrest/FileUtil.h"
#include "wfrest/StatusCode.h"
#include "wfrest/PathUtil.h"

using namespace wfrest;

int FileUtil::size(const std::string &path, OUT size_t *size)
{
    // https://linux.die.net/man/2/stat
    struct stat st;
    memset(&st, 0, sizeof st);
    int ret = stat(path.c_str(), &st);
    int status = StatusOK;
    if(ret == -1)
    {
        *size = 0;
        // fprintf(stderr, "stat error, path = %s\n", path.c_str());
        status = StatusFileStatError;
    } else
    {
        *size = st.st_size;
    }
    return status;
}

bool FileUtil::file_exists(const std::string &path)
{
    return PathUtil::is_file(path);
}
