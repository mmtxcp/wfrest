#ifndef WFREST_PATHUTIL_H_
#define WFREST_PATHUTIL_H_


#if defined __WIN32__ || defined _WIN32 || defined _Windows
#if !defined S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFDIR) == _S_IFDIR)
#endif

#if !defined S_ISREG
#define S_ISREG(m) (((m) & _S_IFREG) == _S_IFREG)
#endif

#endif

#include <string>

namespace wfrest
{
    
class PathUtil
{
public:
    static bool is_dir(const std::string &path);

    static bool is_file(const std::string &path);
    
public:
    static std::string concat_path(const std::string &lhs, const std::string &rhs);

    // filepath = /usr/local/image/test.jpg
    // base = test.jpg
    static std::string base(const std::string &filepath);

    // suffix = jpg
    static std::string suffix(const std::string& filepath);
};

}  // namespace wfrest




#endif // WFREST_PATHUTIL_H_
