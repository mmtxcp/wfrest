#include "workflow/HttpUtil.h"
#include "workflow/MySQLResult.h"
#include "workflow/WFMySQLConnection.h"
#include "workflow/Workflow.h"
#include "workflow/WFTaskFactory.h"

#ifdef WIN32
#include <direct.h>
#include <windows.h>
#include <tchar.h> 
#include <stdio.h>
#include <strsafe.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>
#include <fstream>
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#else
#include <unistd.h>
#endif
#include <algorithm>

#include "HttpMsg.h"
#include "UriUtil.h"
#include "PathUtil.h"
#include "HttpFile.h"
#include "json.hpp"
#include "MysqlUtil.h"
#include "ErrorCode.h"
#include "FileUtil.h"
#include "HttpServerTask.h"

using namespace wfrest;
using namespace protocol;


namespace wfrest
{
using Json = nlohmann::json;
struct ReqData
{
    std::string body;
    std::map<std::string, std::string> form_kv;
    Form form;
    Json json;
};

struct ProxyCtx
{
    std::string url;
    HttpServerTask *server_task;
    bool is_keep_alive;
};
struct MyBuffer2
{
	void* buf = nullptr;
	size_t size = 0;
};

void proxy_http_callback(WFHttpTask *http_task)
{   
    int state = http_task->get_state();
    int error = http_task->get_error();

    auto *proxy_ctx = static_cast<ProxyCtx *>(http_task->user_data);
    
    HttpServerTask *server_task = proxy_ctx->server_task;
    HttpResponse *http_resp = http_task->get_resp();
    HttpResp *server_resp = server_task->get_resp();

    // Some servers may close the socket as the end of http response. 
    if (state == WFT_STATE_SYS_ERROR && error == ECONNRESET)
        state = WFT_STATE_SUCCESS;

    if (state == WFT_STATE_SUCCESS)
    {
        server_task->add_callback([proxy_ctx](HttpTask *server_task)
        {
            HttpResp *server_resp = server_task->get_resp();
            size_t size = server_resp->get_output_body_size();
            if (server_task->get_state() != WFT_STATE_SUCCESS)
            {
                std::string errmsg;
                errmsg.reserve(64);
                errmsg.append(proxy_ctx->url);
                errmsg.append(" : Reply failed: ");
                errmsg.append(strerror(server_task->get_error()));
                errmsg.append(", BodyLength: ");
                errmsg.append(std::to_string(size));
                server_resp->Error(StatusProxyError, errmsg);
            }
            delete proxy_ctx;
        });

        const void *body;
        size_t len;
        // Copy the remote webserver's response, to server response.
        if (http_resp->get_parsed_body(&body, &len))
            http_resp->append_output_body_nocopy(body, len);

        HttpResp resp(std::move(*http_resp));
        *server_resp = std::move(resp);

        if (!proxy_ctx->is_keep_alive)
            server_resp->set_header_pair("Connection", "close");
    }
    else
    {
        const char *err_string;
        int error = http_task->get_error();

        if (state == WFT_STATE_SYS_ERROR)
            err_string = strerror(error);
        else if (state == WFT_STATE_DNS_ERROR)
            err_string = gai_strerror(error);
        else if (state == WFT_STATE_SSL_ERROR)
            err_string = "SSL error";
        else /* if (state == WFT_STATE_TASK_ERROR) */
            err_string = "URL error (Cannot be a HTTPS proxy)";

        std::string errmsg;
        errmsg.reserve(64);
        errmsg.append(proxy_ctx->url);
        errmsg.append(" : Fetch failed. state = ");
        errmsg.append(std::to_string(state));
        errmsg.append(", error = ");
        errmsg.append(std::to_string(http_task->get_error()));
        errmsg.append(" ");
        errmsg.append(err_string);
        server_resp->Error(StatusProxyError, errmsg);
    }
    // move back Request
    auto *server_req = static_cast<HttpRequest *>(server_task->get_req()); 
    *server_req = std::move(*http_task->get_req());
}

Json mysql_concat_json_res(WFMySQLTask *mysql_task)
{
    Json json;
    MySQLResponse *mysql_resp = mysql_task->get_resp();
    MySQLResultCursor cursor(mysql_resp);
    const MySQLField *const *fields;
    std::vector<MySQLCell> arr;

    if (mysql_task->get_state() != WFT_STATE_SUCCESS)
    {
        json["error"] = WFGlobal::get_error_string(mysql_task->get_state(),
                                                    mysql_task->get_error());
        return json;
    }

    do {
        Json result_set;
        if (cursor.get_cursor_status() != MYSQL_STATUS_GET_RESULT &&
            cursor.get_cursor_status() != MYSQL_STATUS_OK)
        {
            break;
        }

        if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT)
        {
            result_set["field_count"] = cursor.get_field_count();
            result_set["rows_count"] = cursor.get_rows_count();
            fields = cursor.fetch_fields();
            std::vector<std::string> fields_name;
            std::vector<std::string> fields_type;
            for (int i = 0; i < cursor.get_field_count(); i++)
            {
                if (i == 0)
                {
                    std::string database = fields[i]->get_db();
                    if(!database.empty())
                        result_set["database"] = std::move(database);
                    result_set["table"] = fields[i]->get_table();
                }
                
                fields_name.push_back(fields[i]->get_name());
                fields_type.push_back(datatype2str(fields[i]->get_data_type()));
            }
            result_set["fields_name"] = fields_name;
            result_set["fields_type"] = fields_type;

            while (cursor.fetch_row(arr))
            {
                Json row;                  
                for (size_t i = 0; i < arr.size(); i++)
                {
                    if (arr[i].is_string())
                    {
                        row.push_back(arr[i].as_string());
                    } 
                    else if (arr[i].is_time() || arr[i].is_datetime()) 
                    {
                        row.push_back(MySQLUtil::to_string(arr[i]));
                    } 
                    else if (arr[i].is_null()) 
                    {
                        row.push_back("NULL");
                    } 
                    else if(arr[i].is_double()) 
                    {
                        row.push_back(arr[i].as_double());
                    } 
                    else if(arr[i].is_float())
                    {
                        row.push_back(arr[i].as_float());
                    }
                    else if(arr[i].is_int())
                    {
                        row.push_back(arr[i].as_int());
                    }
                    else if(arr[i].is_ulonglong())
                    {
                        row.push_back(arr[i].as_ulonglong());
                    }
                }
                result_set["rows"].push_back(row);
            }
        }
        else if (cursor.get_cursor_status() == MYSQL_STATUS_OK)
        {
            result_set["status"] = "OK";
            result_set["affected_rows"] = cursor.get_affected_rows();
            result_set["warnings"] = cursor.get_warnings();
            result_set["insert_id"] = cursor.get_insert_id();
            result_set["info"] = cursor.get_info();
        }
        json["result_set"].push_back(result_set);
    } while (cursor.next_result_set());

    if (mysql_resp->get_packet_type() == MYSQL_PACKET_ERROR)
    {
        json["errcode"] = mysql_task->get_resp()->get_error_code();
        json["errmsg"] = mysql_task->get_resp()->get_error_msg();
    }
    else if (mysql_resp->get_packet_type() == MYSQL_PACKET_OK) 
    {
        json["status"] = "OK";
        json["affected_rows"] = mysql_task->get_resp()->get_affected_rows();
        json["warnings"] = mysql_task->get_resp()->get_warnings();
        json["insert_id"] = mysql_task->get_resp()->get_last_insert_id();
        json["info"] = mysql_task->get_resp()->get_info();
    }
    return json;
}

Json redis_json_res(WFRedisTask *redis_task)
{
    RedisRequest *redis_req = redis_task->get_req();
    RedisResponse *redis_resp = redis_task->get_resp();
    int state = redis_task->get_state();
    int error = redis_task->get_error();
    RedisValue val; 
    ::Json js;
    switch (state)
    {
    case WFT_STATE_SYS_ERROR:
        js["errmsg"] = "system error: " + std::string(strerror(error));
        break;
    case WFT_STATE_DNS_ERROR:
        js["errmsg"] = "DNS error: " + std::string(gai_strerror(error));
        break;
    case WFT_STATE_SSL_ERROR:
        js["errmsg"] = "SSL error: " + std::to_string(error);
        break;
    case WFT_STATE_TASK_ERROR:
        js["errmsg"] = "Task error: " + std::to_string(error);
        break;
    case WFT_STATE_SUCCESS:
        redis_resp->get_result(val);
        if (val.is_error())
        {
            js["errmsg"] = "Error reply. Need a password?\n";
            state = WFT_STATE_TASK_ERROR;
        }
        break;
    }
    std::string cmd;
    std::vector<std::string> params;
    redis_req->get_command(cmd);
    redis_req->get_params(params);
    if (state == WFT_STATE_SUCCESS && cmd == "SET")
    {
        js["status"] = "sucess";
        js["cmd"] = "SET";
        js[params[0]] = params[1];
    }
    if(state == WFT_STATE_SUCCESS && cmd == "GET")
    {
        js["cmd"] = "GET";
        if (val.is_string())
        {
            js[params[0]] = val.string_value();
            js["status"] = "sucess";
        }
        else
        {
            js["errmsg"] = "value is not a string value";
        }
    }
    return js;
}

void mysql_callback(WFMySQLTask *mysql_task)
{
    Json json = mysql_concat_json_res(mysql_task);
    auto *server_resp = static_cast<HttpResp *>(mysql_task->user_data);
    server_resp->String(json.dump());
}

} // namespace wfrest


HttpReq::HttpReq() : req_data_(new ReqData)
{}

HttpReq::~HttpReq()
{
    delete req_data_;
}

std::string &HttpReq::body() const
{
    if (req_data_->body.empty())
    {
        std::string content = protocol::HttpUtil::decode_chunked_body(this);

        const std::string &header = this->header("Content-Encoding");
        int status = StatusOK;
        if (header.find("gzip") != std::string::npos)
        {
            status = Compressor::ungzip(&content, &req_data_->body);
        }
        else
        {
            status = StatusNoUncomrpess;
        }
        if(status != StatusOK)
        {
            req_data_->body = std::move(content);
        }
    }
    return req_data_->body;
}

std::map<std::string, std::string> &HttpReq::form_kv() const
{
    if (content_type_ == APPLICATION_URLENCODED && req_data_->form_kv.empty())
    {
        StringPiece body_piece(this->body());
        req_data_->form_kv = Urlencode::parse_post_kv(body_piece);
    }
    return req_data_->form_kv;
}

Form &HttpReq::form() const
{
    if (content_type_ == MULTIPART_FORM_DATA && req_data_->form.empty())
    {
        StringPiece body_piece(this->body());

        req_data_->form = multi_part_.parse_multipart(body_piece);
    }
    return req_data_->form;
}

Json &HttpReq::json() const
{
    if (content_type_ == APPLICATION_JSON && req_data_->json.empty())
    {
        const std::string &body_content = this->body();
        if (!Json::accept(body_content))
        {
            return req_data_->json;
            // todo : how to let user know the error ?
        }
        req_data_->json = Json::parse(body_content);
    }
    return req_data_->json;
}

const std::string &HttpReq::param(const std::string &key) const
{
    if (route_params_.count(key))
        return route_params_.at(key);
    else
        return string_not_found;
}

bool HttpReq::has_param(const std::string &key) const
{
    return route_params_.count(key) > 0;
}

const std::string &HttpReq::query(const std::string &key) const
{
    if (query_params_.count(key))
        return query_params_.at(key);
    else
        return string_not_found;
}

const std::string &HttpReq::default_query(const std::string &key, const std::string &default_val) const
{
    if (query_params_.count(key))
        return query_params_.at(key);
    else
        return default_val;
}

bool HttpReq::has_query(const std::string &key) const
{
    if (query_params_.find(key) != query_params_.end())
    {
        return true;
    } else
    {
        return false;
    }
}

void HttpReq::fill_content_type()
{
    const std::string &content_type_str = header("Content-Type");
    content_type_ = ContentType::to_enum(content_type_str);

    if (content_type_ == MULTIPART_FORM_DATA)
    {
        // if type is multipart form, we reserve the boudary first
        const char *boundary = strstr(content_type_str.c_str(), "boundary=");
        if (boundary == nullptr)
        {
            return;
        }
        boundary += strlen("boundary=");
        StringPiece boundary_piece(boundary);

        StringPiece boundary_str = StrUtil::trim_pairs(boundary_piece, R"(""'')");
        multi_part_.set_boundary(boundary_str.as_string());
    }
}

const std::string &HttpReq::header(const std::string &key) const
{
    const auto it = headers_.find(key);

    if (it == headers_.end() || it->second.empty())
        return string_not_found;

    return it->second[0];
}

bool HttpReq::has_header(const std::string &key) const
{
    return headers_.count(key) > 0;
}

void HttpReq::fill_header_map()
{
    http_header_cursor_t cursor;
    struct protocol::HttpMessageHeader header;

    http_header_cursor_init(&cursor, this->get_parser());
    while (http_header_cursor_next(&header.name, &header.name_len,
                                   &header.value, &header.value_len,
                                   &cursor) == 0)
    {
        std::string key(static_cast<const char *>(header.name), header.name_len);

        headers_[key].emplace_back(static_cast<const char *>(header.value), header.value_len);
    }

    http_header_cursor_deinit(&cursor);
}

const std::map<std::string, std::string> &HttpReq::cookies() const
{
    if (cookies_.empty() && this->has_header("Cookie"))
    {
        const std::string &cookie = this->header("Cookie");
        StringPiece cookie_piece(cookie);
        cookies_ = std::move(HttpCookie::split(cookie_piece));
    }
    return cookies_;
}

const std::string &HttpReq::cookie(const std::string &key) const
{
    if(cookies_.empty()) 
    {
        this->cookies();
    }
    if(cookies_.find(key) != cookies_.end())
    {
        return cookies_[key];
    }
    return string_not_found;
}

HttpReq::HttpReq(HttpReq&& other)
    : HttpRequest(std::move(other)),
    content_type_(other.content_type_),
    route_match_path_(std::move(other.route_match_path_)),
    route_full_path_(std::move(other.route_full_path_)),
    route_params_(std::move(other.route_params_)),
    query_params_(std::move(other.query_params_)),
    cookies_(std::move(other.cookies_)),
    multi_part_(std::move(other.multi_part_)),
    headers_(std::move(other.headers_)),
    parsed_uri_(std::move(other.parsed_uri_))
{
    req_data_ = other.req_data_;
    other.req_data_ = nullptr;
}

HttpReq &HttpReq::operator=(HttpReq&& other)
{
    HttpRequest::operator=(std::move(other));
    content_type_ = other.content_type_;

    req_data_ = other.req_data_;
    other.req_data_ = nullptr;

    route_match_path_ = std::move(other.route_match_path_);
    route_full_path_ = std::move(other.route_full_path_);
    route_params_ = std::move(other.route_params_);
    query_params_ = std::move(other.query_params_);
    cookies_ = std::move(other.cookies_);
    multi_part_ = std::move(other.multi_part_);
    headers_ = std::move(other.headers_);
    parsed_uri_ = std::move(other.parsed_uri_);

    return *this;
}

void HttpResp::String(const std::string &str)
{
    auto *compress_data = new std::string;
    int ret = this->compress(&str, compress_data);
    if(ret != StatusOK)   
    {
        this->append_output_body(static_cast<const void *>(str.c_str()), str.size());
    } else 
    {
        this->append_output_body_nocopy(compress_data->c_str(), compress_data->size());
    }
    task_of(this)->add_callback([compress_data](HttpTask *) { delete compress_data; });
}

void HttpResp::String(std::string &&str)
{
    auto *data = new std::string;
    int ret = this->compress(&str, data);
    if(ret != StatusOK)
    {   
        *data = std::move(str);
    } 
    this->append_output_body_nocopy(data->c_str(), data->size());
    task_of(this)->add_callback([data](HttpTask *) { delete data; });
}

void HttpResp::String(const MultiPartEncoder &multi_part_encoder)
{
    MultiPartEncoder *encoder = new MultiPartEncoder(multi_part_encoder);
    this->String(encoder);
} 

void HttpResp::String(MultiPartEncoder &&multi_part_encoder)
{
    MultiPartEncoder *encoder = new MultiPartEncoder(std::move(multi_part_encoder));
    this->String(encoder);
}
#ifdef WIN32
void fileread2(const std::string& pathname, void* buf, size_t count, off_t offset)
{
	try
	{
		std::ifstream outFile(pathname, std::ios::binary);
		if (!outFile.is_open())
		{
			return;
		}
		outFile.seekg(offset);
		outFile.read((char*)buf, count);
		outFile.close();
	}
	catch (std::exception& e)
	{

	}

}
#endif // WIN32

void HttpResp::String(MultiPartEncoder *encoder) 
{   
    const std::string &boudary = encoder->boundary();
    this->headers["Content-Type"] = "multipart/form-data; boundary=" + boudary;
    
    HttpServerTask *server_task = task_of(this);
    SeriesWork *series = series_of(server_task);

    const MultiPartEncoder::FileList &file_list = encoder->files();
    size_t file_cnt = file_list.size();
    assert(file_cnt >= 0);
	for (const auto& file : file_list)
	{
		file_cnt--;
		if (!PathUtil::is_file(file.second))
		{
			fprintf(stderr, "[Error] Not a File : %s\n", file.second.c_str());
			continue;
		}
		size_t file_size;
		int ret = FileUtil::size(file.second, OUT & file_size);
		if (ret != StatusOK)
		{
			fprintf(stderr, "[Error] Invalid File : %s\n", file.second.c_str());
			continue;
		}
		int fd = open(file.second.c_str(), O_RDONLY);
		if (fd < 0){
			continue;
		}
		
        void *buf = malloc(file_size);
#ifdef WIN32
		auto funcallback =
			[&file, &boudary](WFGoTask* pread_task) {
			HttpServerTask* server_task = task_of(pread_task);
			MyBuffer2* mybuf = (MyBuffer2*)server_task->user_data;
			
			SeriesWork* series = series_of(pread_task);
			std::string* content = static_cast<std::string*>(series->get_context());
			if (pread_task->get_state() != WFT_STATE_SUCCESS )
			{
				fprintf(stderr, "Read %s Error\n", file.second.c_str());
			}
			else
			{
				std::string file_suffix = PathUtil::suffix(file.second);
				std::string file_type = ContentType::to_str_by_suffix(file_suffix);
				content->append("\r\n--");
				content->append(boudary);
				content->append("\r\nContent-Disposition: form-data; name=\"");
				content->append(file.first);
				content->append("\"; filename=\"");
				content->append(PathUtil::base(file.second));
				content->append("\"\r\nContent-Type: ");
				content->append(file_type);
				content->append("\r\n\r\n");
				if (mybuf)
				{
					content->append(static_cast<char*>(mybuf->buf), mybuf->size);
				}
				
			}
			// last one, send the content
			if (pread_task->user_data) {
				content->append("\r\n--");
				content->append(boudary);
				content->append("--\r\n");
				HttpResp* resp = static_cast<HttpResp*>(pread_task->user_data);
				resp->append_output_body_nocopy(content->c_str(), content->size());
			}
		};
		MyBuffer2* mybuf = new MyBuffer2();
		mybuf->buf = buf;
		mybuf->size = file_size;
		WFGoTask* pread_task = WFTaskFactory::create_go_task("MultiPartEncoder",fileread2,file.second,
			buf, file_size, 0);
			pread_task->set_callback(funcallback);
			server_task->user_data = (void*)mybuf;
#else
		auto funcallback =
			[&file, &boudary](WFFileIOTask* pread_task) {
			FileIOArgs* args = pread_task->get_args();
			long ret = pread_task->get_retval();
			close(args->fd);
			SeriesWork* series = series_of(pread_task);
			std::string* content = static_cast<std::string*>(series->get_context());
			if (pread_task->get_state() != WFT_STATE_SUCCESS || ret < 0)
			{
				fprintf(stderr, "Read %s Error\n", file.second.c_str());
			}
			else
			{
				std::string file_suffix = PathUtil::suffix(file.second);
				std::string file_type = ContentType::to_str_by_suffix(file_suffix);
				content->append("\r\n--");
				content->append(boudary);
				content->append("\r\nContent-Disposition: form-data; name=\"");
				content->append(file.first);
				content->append("\"; filename=\"");
				content->append(PathUtil::base(file.second));
				content->append("\"\r\nContent-Type: ");
				content->append(file_type);
				content->append("\r\n\r\n");
				content->append(static_cast<char*>(args->buf), ret);
			}
			// last one, send the content
			if (pread_task->user_data) {
				content->append("\r\n--");
				content->append(boudary);
				content->append("--\r\n");
				HttpResp* resp = static_cast<HttpResp*>(pread_task->user_data);
				resp->append_output_body_nocopy(content->c_str(), content->size());
			}
		};
		WFFileIOTask* pread_task = WFTaskFactory::create_pread_task(fd,
			buf, file_size, 0, funcallback);
#endif // WIN32

       
        if(file_cnt == 0)
        {
            pread_task->user_data = this;
        }
        server_task->add_callback([buf](const HttpTask *server_task)
                                {
                                    free(buf);
                                });
        series->push_back(pread_task);
    }
    
    std::string *content = new std::string;
    series->set_context(content);
    series->set_callback([encoder](const SeriesWork *series) {
        delete encoder;
        delete static_cast<std::string *>(series->get_context());
    });

    const MultiPartEncoder::ParamList &param_list = encoder->params();
    for(const auto &param : param_list) {
        content->append("\r\n--");
        content->append(boudary);
        content->append("\r\nContent-Disposition: form-data; name=\"");
        content->append(param.first);
        content->append("\"\r\n\r\n");
        content->append(param.second);
    } 
}

int HttpResp::compress(const std::string * const data, std::string *compress_data)
{
    int status = StatusOK;
    if (headers.find("Content-Encoding") != headers.end())
    {
        if (headers["Content-Encoding"].find("gzip") != std::string::npos)
        {
            status = Compressor::gzip(data, compress_data);
        }
    } else 
    {
        status = StatusNoComrpess;
    }
    return status;
}

void HttpResp::Error(int error_code)
{
    this->Error(error_code, "");
}

void HttpResp::Error(int error_code, const std::string &errmsg)
{
    int status_code = 503;
    switch (error_code)
    {
    case StatusNotFound:
    case StatusRouteVerbNotImplment:
    case StatusRouteNotFound:
        status_code = 404;
        break;
    default:
        break;
    }
    this->headers["Content-Type"] = "application/json";
    this->set_status(status_code); 
    ::Json js;
    std::string resp_msg = error_code_to_str(error_code);
    if(!errmsg.empty()) resp_msg = resp_msg + " : " + errmsg;
    js["errmsg"] = resp_msg;

    this->Json(js);
}
void HttpResp::File(const std::string &path)
{
    this->File(path, 0, -1);
}

void HttpResp::File(const std::string &path, size_t start)
{
    this->File(path, start, -1);
}

void HttpResp::File(const std::string &path, size_t start, size_t end)
{
    int ret = HttpFile::send_file(path, start, end, this);
    if(ret != StatusOK)
    {
        this->Error(ret);
    }
}

void HttpResp::set_status(int status_code)
{
    protocol::HttpUtil::set_response_status(this, status_code);
}

void HttpResp::Save(const std::string &file_dst, const std::string &content)
{
    HttpFile::save_file(file_dst, content, this);
}

void HttpResp::Save(const std::string &file_dst, std::string &&content)
{
    HttpFile::save_file(file_dst, std::move(content), this);
}

void HttpResp::Save(const std::string &file_dst, const std::string &content, const std::string &notify_msg)
{
    HttpFile::save_file(file_dst, content, this, notify_msg);
}

void HttpResp::Save(const std::string &file_dst, const std::string &content, std::string &&notify_msg)
{
    HttpFile::save_file(file_dst, content, this, std::move(notify_msg));
}

void HttpResp::Save(const std::string &file_dst, std::string &&content, const std::string &notify_msg)
{
    HttpFile::save_file(file_dst, std::move(content), this, notify_msg);
}

void HttpResp::Save(const std::string &file_dst, std::string &&content, std::string &&notify_msg)
{
    HttpFile::save_file(file_dst, std::move(content), this, std::move(notify_msg));
}

void HttpResp::Json(const ::Json &json)
{
    // The header value itself does not allow for multiple values, 
    // and it is also not allowed to send multiple Content-Type headers
    // https://stackoverflow.com/questions/5809099/does-the-http-protocol-support-multiple-content-types-in-response-headers
    this->headers["Content-Type"] = "application/json";
    this->String(json.dump());
}

void HttpResp::Json(const std::string &str)
{
    if (!Json::accept(str))
    {
        this->Error(StatusJsonInvalid);
        return;
    }
    this->headers["Content-Type"] = "application/json";
    this->String(str);
}

void HttpResp::set_compress(const Compress &compress)
{
    // https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Encoding
    headers["Content-Encoding"] = compress_method_to_str(compress);
}

int HttpResp::get_state() const
{
    HttpServerTask *server_task = task_of(this);
    return server_task->get_state();   
}

int HttpResp::get_error() const
{
    HttpServerTask *server_task = task_of(this);
    return server_task->get_error();   
}
int startsWith(const std::string& s, const std::string& sub) {
	return s.find(sub) == 0 ? 1 : 0;
}

int endsWith(const std::string& s, const std::string& sub) {
	return s.rfind(sub) == (s.length() - sub.length()) ? 1 : 0;
}

void HttpResp::Http(const std::string &url, int redirect_max, size_t size_limit)
{
    HttpServerTask *server_task = task_of(this);
    HttpReq *server_req = server_task->get_req();

	//�����ַ����
	std::string request_uri = server_req->get_request_uri();  // or can't parse URI
	std::string full_path = server_req->full_path();
	std::string current_path = server_req->current_path();
	std::string match_path = server_req->match_path();
	if (match_path.empty())
	{
		if (full_path.size() <= request_uri.size())
			request_uri = request_uri.substr(full_path.size(), request_uri.size() - full_path.size());
		else
		{
			request_uri = match_path;
		}
	}
	else
	{
		if (endsWith(full_path, "/*"))
		{
			request_uri = "/" + match_path;
		}
		else
		{
			request_uri = request_uri.substr(current_path.size(), request_uri.size() - current_path.size());
		}
	}
	

    std::string http_url = url + request_uri;
	if (strncasecmp(url.c_str(), "http://", 7) != 0 &&
		strncasecmp(url.c_str(), "https://", 8) != 0)
	{
		http_url = "http://" + http_url;
	}
    WFHttpTask *http_task = WFTaskFactory::create_http_task(http_url, 
                                                            redirect_max, 
                                                            0, 
                                                            proxy_http_callback);
    auto *proxy_ctx = new ProxyCtx;
    proxy_ctx->url = http_url;
    proxy_ctx->server_task = server_task;
    proxy_ctx->is_keep_alive = server_req->is_keep_alive();
    http_task->user_data = proxy_ctx;

    const void *body;
	size_t len;      

    ParsedURI uri;
    // fprintf(stderr, "%s\n", http_url.c_str());
    if (URIParser::parse(http_url, uri) < 0)
    {
        server_task->get_resp()->set_status(HttpStatusBadRequest);
        return;
    }

    std::string route;
    if (uri.path && uri.path[0])
    {
        route = uri.path;
    } else 
    {
        route = "/";
    }

	server_req->set_request_uri(route);
	server_req->get_parsed_body(&body, &len);
	server_req->append_output_body_nocopy(body, len);
    // Keep parts unique to HttpReq
    HttpRequest *server_req_cast = static_cast<HttpRequest *>(server_req);
	*http_task->get_req() = std::move(*server_req_cast);  
    http_task->get_resp()->set_size_limit(size_limit);
	**server_task << http_task;
}

void HttpResp::MySQL(const std::string &url, const std::string &sql)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, mysql_callback);
    mysql_task->get_req()->set_query(sql);
    mysql_task->user_data = this;
    this->add_task(mysql_task);
}

void HttpResp::MySQL(const std::string &url, const std::string &sql, const MySQLJsonFunc &func)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, 
    [func](WFMySQLTask *mysql_task)
    {
        ::Json json = mysql_concat_json_res(mysql_task);
        func(&json);
    });

    mysql_task->get_req()->set_query(sql);
    this->add_task(mysql_task);
}

void HttpResp::MySQL(const std::string &url, const std::string &sql, const MySQLFunc &func)
{
    WFMySQLTask *mysql_task = WFTaskFactory::create_mysql_task(url, 0, 
    [func](WFMySQLTask *mysql_task)
    {
        if (mysql_task->get_state() != WFT_STATE_SUCCESS)
        {
            std::string errmsg = WFGlobal::get_error_string(mysql_task->get_state(),
                                                mysql_task->get_error());
            auto *server_resp = static_cast<HttpResp *>(mysql_task->user_data);
            server_resp->String(std::move(errmsg));
            return;
        }
        MySQLResponse *mysql_resp = mysql_task->get_resp();
        MySQLResultCursor cursor(mysql_resp);
        func(&cursor);
    });
    mysql_task->get_req()->set_query(sql);
    mysql_task->user_data = this;
    this->add_task(mysql_task);
}

void HttpResp::Redis(const std::string &url, const std::string &command,
        const std::vector<std::string>& params)
{
    WFRedisTask *redis_task = WFTaskFactory::create_redis_task(url, 2, [this](WFRedisTask *redis_task) 
    {
        ::Json js = redis_json_res(redis_task);
        this->Json(js);
    });
	redis_task->get_req()->set_request(command, params);
    this->add_task(redis_task);
}

void HttpResp::Redis(const std::string &url, const std::string &command,
        const std::vector<std::string>& params, const RedisFunc &func)
{
    WFRedisTask *redis_task = WFTaskFactory::create_redis_task(url, 2, [func](WFRedisTask *redis_task) 
    {
        ::Json js = redis_json_res(redis_task);
        func(&js);
    });
	redis_task->get_req()->set_request(command, params);
    this->add_task(redis_task);
}

void HttpResp::add_task(SubTask *task)
{
    HttpServerTask *server_task = task_of(this);
    **server_task << task;
}

HttpResp::HttpResp(HttpResp&& other)
    : HttpResponse(std::move(other)),
    headers(std::move(other.headers)),
    cookies_(std::move(other.cookies_))
{
    user_data = other.user_data;
    other.user_data = nullptr;
}

HttpResp &HttpResp::operator=(HttpResp&& other)
{
    HttpResponse::operator=(std::move(other));
    headers = std::move(other.headers);
    user_data = other.user_data;
    other.user_data = nullptr;
    cookies_ = std::move(other.cookies_);
    return *this;
}

