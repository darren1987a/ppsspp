#include <string>
#include <thread>

#include "Common/Buffer.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/HTTPServer.h"
#include "Common/Net/Sinks.h"

#include "UnitTest.h"

// An in-process server echoes the X-Cloud-Test request header, proving extraHeaders reach the wire.
static bool TestCloudSaveHttpHeaders() {
	http::Server server(new NewThreadExecutor());
	std::string received;
	server.RegisterHandler("/echo", [&received](const http::ServerRequest &request) {
		request.GetHeader("x-cloud-test", &received);
		request.WriteHttpResponseHeader("1.0", 200, received.size(), "text/plain");
		request.Out()->Push(received);
	});
	EXPECT_TRUE(server.Listen(0, "unittest", net::DNSType::IPV4));
	const int port = server.Port();
	std::thread serveThread([&server] { server.RunSlice(5.0); });

	bool cancelled = false;
	http::Client client(nullptr);
	EXPECT_TRUE(client.Resolve("127.0.0.1", port));
	EXPECT_TRUE(client.Connect(2, 5.0, &cancelled));
	http::RequestParams req("/echo", "*/*");
	req.extraHeaders = "X-Cloud-Test: hello-header\r\n";
	Buffer output;
	net::RequestProgress progress(&cancelled);  // Client::GET requires one.
	int code = client.GET(req, &output, &progress);
	serveThread.join();

	EXPECT_EQ_INT(code, 200);
	std::string body;
	output.TakeAll(&body);
	EXPECT_EQ_STR(body, std::string("hello-header"));
	return true;
}

bool TestCloudSave() {
	if (!TestCloudSaveHttpHeaders())
		return false;
	return true;
}
