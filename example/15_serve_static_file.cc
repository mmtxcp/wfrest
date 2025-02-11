#include "workflow/WFFacilities.h"
#include <csignal>
#include "wfrest/HttpServer.h"

using namespace wfrest;

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo)
{
    wait_group.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer svr;
    /*svr.Static("/static", "./www/static");

    svr.Static("/public", "./www");

    svr.Static("/", "./www/index.html");*/
	svr.Static("/xfz", "D:/KQGIS/data/三维数据/环境1_小房子");

	svr.Static("/bm", "D:/KQGIS/data/三维数据/社区新白模3");


    if (svr.start(8888) == 0)
    {
        svr.list_routes();
        wait_group.wait();
        svr.stop();
    } else
    {
        fprintf(stderr, "Cannot start server");
        exit(1);
    }
    return 0;
}
