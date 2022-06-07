#include "workflow/WFTaskFactory.h"

#include <sys/stat.h>

#include "HttpFile.h"
#include "HttpMsg.h"
#include "PathUtil.h"
#include "HttpServerTask.h"
#include "FileUtil.h"
#include "ErrorCode.h"

#ifdef WIN32
#include <windows.h>
#include <tchar.h> 
#include <stdio.h>
#include <strsafe.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>
#include <fstream>
//#pragma comment(lib, "User32.lib")
#endif // WIN32

using namespace wfrest;

namespace
{

struct SaveFileContext 
{
    std::string content;
    std::string notify_msg;
};
struct MyBuffer
{
	void* buf=nullptr;
	size_t size=0;
};
/*
We do not occupy any thread to read the file, but generate an asynchronous file reading task
and reply to the request after the reading is completed.

We need to read the whole data into the memory before we start replying to the message.
Therefore, it is not suitable for transferring files that are too large.

todo : Any better way to transfer large File?
*/
void pread_callback(WFFileIOTask *pread_task)
{
    FileIOArgs *args = pread_task->get_args();
    long ret = pread_task->get_retval();
    auto *resp = static_cast<HttpResp *>(pread_task->user_data);
	close(args->fd);
    if (pread_task->get_state() != WFT_STATE_SUCCESS || ret < 0)
    {
       
		resp->set_status_code("503");
		resp->append_output_body("<html>503 Internal Server Error.</html>");
    } else
    {
        resp->append_output_body_nocopy(args->buf, ret);
    }
}
#ifdef WIN32
void pread_callback2(WFGoTask* pread_task)
{
	HttpServerTask* server_task = task_of(pread_task);
	auto* resp = static_cast<HttpResp*>(pread_task->user_data);
	if (pread_task->get_state() != WFT_STATE_SUCCESS )
	{

		resp->set_status_code("503");
		resp->append_output_body("<html>503 Internal Server Error.</html>");
	}
	else
	{
		MyBuffer* mybuf = (MyBuffer*)server_task->user_data;
		if (mybuf)
		{
			resp->append_output_body_nocopy(mybuf->buf, mybuf->size);
			delete mybuf;
			mybuf = nullptr;
		}
		
	}
}
void pwrite_callback2(WFGoTask* pwrite_task)
{

	HttpServerTask* server_task = task_of(pwrite_task);
	HttpResp* resp = server_task->get_resp();
	auto* save_context = static_cast<SaveFileContext*>(pwrite_task->user_data);

	if (pwrite_task->get_state() != WFT_STATE_SUCCESS )
	{
		resp->Error(StatusFileWriteError);
	}
	else
	{
		if (save_context->notify_msg.empty())
		{
			resp->append_output_body_nocopy("Save File success\n", 18);
		}
		else
		{
			resp->append_output_body_nocopy(save_context->notify_msg.c_str(), save_context->notify_msg.size());
		}
	}
}
void filewriter(const std::string& path, SaveFileContext*& save_context)
{
	try
	{
		//打开文件
		std::ofstream outFile(path);
		if (!outFile.is_open())
		{
			save_context->notify_msg = "open file faild.";
		}
		outFile << save_context->content;
		outFile.close();
	}
	catch (std::exception& e)
	{
		save_context->notify_msg = e.what();
	}

}

void fileread(const std::string& pathname,	void* buf,	size_t count,	off_t offset)
{
	try
	{
		std::ifstream outFile(pathname);
		if (!outFile.is_open())
		{
			return ;
		}
		outFile.seekg(offset);
		outFile.read((char*)buf,count);
		outFile.close();
	}
	catch (std::exception& e)
	{
		
	}

}
#endif // WIN32
void pwrite_callback(WFFileIOTask* pwrite_task)
{
	FileIOArgs* args = pwrite_task->get_args();
	long ret = pwrite_task->get_retval();
	HttpServerTask* server_task = task_of(pwrite_task);
	HttpResp* resp = server_task->get_resp();
	auto* save_context = static_cast<SaveFileContext*>(pwrite_task->user_data);
	close(args->fd);

	if (pwrite_task->get_state() != WFT_STATE_SUCCESS || ret < 0)
	{
		resp->Error(StatusFileWriteError);
	}
	else
	{
		if (save_context->notify_msg.empty())
		{
			resp->append_output_body_nocopy("Save File success\n", 18);
		}
		else
		{
			resp->append_output_body_nocopy(save_context->notify_msg.c_str(), save_context->notify_msg.size());
		}
	}
}




}  // namespace

// note : [start, end)
int HttpFile::send_file(const std::string &path, size_t file_start, size_t file_end, HttpResp *resp)
{
    if(!PathUtil::is_file(path))
    {
        return StatusNotFound;
    }
    int start = file_start;
    int end = file_end;
    if (end == -1 || start < 0)
    {
        size_t file_size;
        int ret = FileUtil::size(path, OUT &file_size);

        if (ret != StatusOK)
        {
            return ret;
        }
        if (end == -1) end = file_size;
        if (start < 0) start = file_size + start;
    }

    if (end <= start)
    {
        return StatusFileRangeInvalid;
    }

    http_content_type content_type = CONTENT_TYPE_NONE;
    std::string suffix = PathUtil::suffix(path);
    if(!suffix.empty())
    {
        content_type = ContentType::to_enum_by_suffix(suffix);
    }
    if (content_type == CONTENT_TYPE_NONE || content_type == CONTENT_TYPE_UNDEFINED) {
        content_type = APPLICATION_OCTET_STREAM;
    }
    resp->headers["Content-Type"] = ContentType::to_str(content_type);
	int fd = open(path.c_str(), O_RDONLY);
	if (fd >= 0)
	{
		size_t size = end - start;
		void* buf = malloc(size);

		HttpServerTask* server_task = task_of(resp);
		server_task->add_callback([buf](HttpTask* server_task)
			{
				free(buf);
			});
		// https://datatracker.ietf.org/doc/html/rfc7233#section-4.2
		// Content-Range: bytes 42-1233/1234
		resp->headers["Content-Range"] = "bytes " + std::to_string(start)
			+ "-" + std::to_string(end)
			+ "/" + std::to_string(size);
#ifdef WIN32
		WFGoTask* pread_task = WFTaskFactory::create_go_task(path, fileread, path,
			buf,
			size,
			static_cast<off_t>(start));
		pread_task->set_callback(pread_callback2);
		//我的添加
		MyBuffer* mybuf = new MyBuffer();
		mybuf->buf = buf;
		mybuf->size = size;
	
		server_task->user_data = (void*)mybuf;	/* to free() in callback() */
#else
		WFFileIOTask* pread_task = WFTaskFactory::create_pread_task(fd,
			buf,
			size,
			static_cast<off_t>(start),
			pread_callback);
#endif
		pread_task->user_data = resp;
		
		**server_task << pread_task;
		return StatusOK;

	}

	resp->Error(StatusFileReadError);
	return StatusFileReadError;

}

void HttpFile::save_file(const std::string &dst_path, const std::string &content, HttpResp *resp)
{
    HttpFile::save_file(dst_path, content, resp, "");
}

void HttpFile::save_file(const std::string &dst_path, const std::string &content,
                                        HttpResp *resp, const std::string &notify_msg) 
{
	int fd = open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
	{
		resp->Error(StatusFileWriteError);
		return;
	}
    HttpServerTask *server_task = task_of(resp);

    auto *save_context = new SaveFileContext; 
    save_context->content = content;    // copy
    save_context->notify_msg = notify_msg;  // copy
#ifdef WIN32
	WFGoTask* pwrite_task = WFTaskFactory::create_go_task(dst_path, filewriter, dst_path,
		std::ref(save_context));
	pwrite_task->set_callback(pwrite_callback2);
#else
    WFFileIOTask *pwrite_task = WFTaskFactory::create_pwrite_task(fd,
                                                                  static_cast<const void *>(save_context->content.c_str()),
                                                                  save_context->content.size(),
                                                                  0,
                                                                  pwrite_callback);
#endif
	**server_task << pwrite_task;
    server_task->add_callback([save_context](HttpTask *) {
        delete save_context;
    });
    pwrite_task->user_data = save_context;
}

void HttpFile::save_file(const std::string &dst_path, const std::string &content,
                                        HttpResp *resp, std::string &&notify_msg) 
{
	int fd = open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
	{
		resp->Error(StatusFileWriteError);
		return;
	}
    HttpServerTask *server_task = task_of(resp);

    auto *save_context = new SaveFileContext; 
    save_context->content = content;    // copy
    save_context->notify_msg = std::move(notify_msg);  
#ifdef WIN32
	WFGoTask* pwrite_task = WFTaskFactory::create_go_task(dst_path, filewriter, dst_path,
		std::ref(save_context));
	pwrite_task->set_callback(pwrite_callback2);
#else
    WFFileIOTask *pwrite_task = WFTaskFactory::create_pwrite_task(fd,
                                                                  static_cast<const void *>(save_context->content.c_str()),
                                                                  save_context->content.size(),
                                                                  0,
                                                                  pwrite_callback);
#endif
    **server_task << pwrite_task;
    server_task->add_callback([save_context](HttpTask *) {
        delete save_context;
    });
    pwrite_task->user_data = save_context;
}

void HttpFile::save_file(const std::string &dst_path, std::string &&content, HttpResp *resp)
{
    HttpFile::save_file(dst_path, std::move(content), resp, "");   
}

void HttpFile::save_file(const std::string &dst_path, std::string &&content, 
                                        HttpResp *resp, const std::string &notify_msg) 
{
	int fd = open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
	{
		resp->Error(StatusFileWriteError);
		return;
	}
    HttpServerTask *server_task = task_of(resp);

    auto *save_context = new SaveFileContext; 
    save_context->content = std::move(content);  
    save_context->notify_msg = notify_msg;  // copy
#ifdef WIN32
	WFGoTask* pwrite_task = WFTaskFactory::create_go_task(dst_path, filewriter, dst_path,
		std::ref(save_context));
	pwrite_task->set_callback(pwrite_callback2);
#else
	WFFileIOTask* pwrite_task = WFTaskFactory::create_pwrite_task(fd,
		static_cast<const void*>(save_context->content.c_str()),
		save_context->content.size(),
		0,
		pwrite_callback);
#endif
   
    **server_task << pwrite_task;
    server_task->add_callback([save_context](HttpTask *) {
        delete save_context;
    });
    pwrite_task->user_data = save_context;
}
#ifdef WIN32

void HttpFile::save_file(const std::string& dst_path, std::string&& content,
	HttpResp* resp, std::string&& notify_msg)
{
	int fd = open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
	{
		resp->Error(StatusFileWriteError);
		return;
	}
	HttpServerTask* server_task = task_of(resp);

	auto* save_context = new SaveFileContext;
	save_context->content = std::move(content);
	save_context->notify_msg = std::move(notify_msg);

	WFGoTask* pwrite_task = WFTaskFactory::create_go_task(dst_path,filewriter, dst_path,
		std::ref(save_context));
	pwrite_task->set_callback(pwrite_callback2);
	**server_task << pwrite_task;
	server_task->add_callback([save_context](HttpTask*) {
		delete save_context;
		});
	pwrite_task->user_data = save_context;
}
#else
void HttpFile::save_file(const std::string& dst_path, std::string&& content,
	HttpResp* resp, std::string&& notify_msg)
{
	int fd = open(dst_path.c_str(), O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
	{
		resp->Error(StatusFileWriteError);
		return;
	}
	HttpServerTask* server_task = task_of(resp);

	auto* save_context = new SaveFileContext;
	save_context->content = std::move(content);
	save_context->notify_msg = std::move(notify_msg);

	WFFileIOTask* pwrite_task = WFTaskFactory::create_pwrite_task(fd,
		static_cast<const void*>(save_context->content.c_str()),
		save_context->content.size(),
		0,
		pwrite_callback);
	**server_task << pwrite_task;
	server_task->add_callback([save_context](HttpTask*) {
		delete save_context;
		});
	pwrite_task->user_data = save_context;
}
#endif // WIN32


