/*
// webtunnelagent.h
//
// The WebTunnel Agent C API
//
// Copyright (c) 2013-2023, Applied Informatics Software Engineering GmbH.
// All rights reserved.
//
// SPDX-License-Identifier:	BSL-1.0
*/


#ifndef WebTunnelAgent_INCLUDED
#define WebTunnelAgent_INCLUDED


/*
// The following block is the standard way of creating macros which make exporting
// from a DLL simpler. All files within this DLL are compiled with the WebTunnelAgent_EXPORTS
// symbol defined on the command line. this symbol should not be defined on any project
// that uses this DLL. This way any other project whose source files include this file see
// WebTunnelAgent_API functions as being imported from a DLL, wheras this DLL sees symbols
// defined with this macro as being exported.
*/
#if defined(_WIN32) && defined(WebTunnelAgent_DLL)
	#if defined(WebTunnelAgent_EXPORTS)
		#define WebTunnelAgent_API __declspec(dllexport)
	#else
		#define WebTunnelAgent_API __declspec(dllimport)
	#endif
#endif


#if !defined(WebTunnelAgent_API)
	#define WebTunnelAgent_API
#endif


/*
// Automatically link WebTunnelAgentCAPI library.
*/
#if defined(_MSC_VER) && defined(WebTunnelAgent_DLL)
	#if !defined(WebTunnelAgent_EXPORTS)
		#if defined(_DEBUG)
			#pragma comment(lib, "WebTunnelAgentd.lib")
		#else
			#pragma comment(lib, "WebTunnelAgent.lib")
		#endif
	#endif
#endif


/*
// Agent API
*/


#ifdef __cplusplus
extern "C" {
#endif


typedef enum webtunnel_agent_result
{
	webtunnel_agent_result_ok = 0,
	webtunnel_agent_result_error = 1,
	webtunnel_agent_result_not_supported = 2
} webtunnel_agent_result;


typedef enum webtunnel_agent_port_type
{
	webtunnel_port_http = 0,
	webtunnel_port_https = 1,
	webtunnel_port_ssh = 2,
	webtunnel_port_vnc = 3,
	webtunnel_port_rdp = 4,
	webtunnel_port_app = 5,
	webtunnel_port_other = 6
} webtunnel_agent_port_type;


typedef enum webtunnel_agent_status
{
	webtunnel_agent_status_disconnected = 0,
	webtunnel_agent_status_connected = 1,
	webtunnel_agent_status_error = 2,
	webtunnel_agent_status_unknown = 3
} webtunnel_agent_status;


typedef struct webtunnel_agent_port_spec
{
	unsigned short port;
	unsigned short type;
} webtunnel_agent_port_spec;


typedef void* webtunnel_agent;


/*
// webtunnel_agent_init
//
// Initialize webtunnel agent library.
// Must be called before any other functions.
// Returns webunnel_agent_ok if successful, or
// webtunnel_agent_error otherwise.
*/
int WebTunnelAgent_API webtunnel_agent_init(void);


/*
// webtunnel_agent_cleanup
//
// Cleanup webtunnel agent library.
// Should be called when the library is no longer being used
// to cleanup internal state.
*/
void WebTunnelAgent_API webtunnel_agent_cleanup(void);


/*
// webtunnel_agent_configure_timeouts
//
// Configure timeouts for WebTunnel connections.
//
// All timeouts are in seconds.
//
// connect_timeout is the timeout for setting up the initial HTTP connection
// to the reflector server.
//
// remote_timeout specifies the timeout of the tunnel connection to the reflector service.
// If no data has been received for this period, the client will send a PING
// message to the server. If the server does not reply to the PING, the connection
// will be closed.
//
// local_timeout specifies the timeout of the local socket connection.
// If no data has been received for this period, the connection will be closed.
*/
int WebTunnelAgent_API webtunnel_agent_configure_timeouts(int connect_timeout, int remote_timeout, int local_timeout);


/*
// webtunnel_agent_configure_tls
//
// Sets up SSL/TLS parameters for the connection to the
// reflector server.
//
// If accept_unknown_cert is true, any server certificate, even without
// a valid chain, will be accepted.
//
// If extended_verification is true, extended certificate verification
// will be performed, which means that the certificate must contain the
// fully qualified domain name of the reflector server.
//
// A list of ciphers can be given in ciphers, using OpenSSL syntax
// (e.g., "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH"). Can be NULL to use
// the default.
//
// ca_location contains the path to the file or directory containing
// the CA/root certificates. Can be NULL to use the built-in root
// certificates.
//
// Returns webtunnel_agent_ok if successful, or webtunnel_client_error if an error
// occured, or webtunnel_agent_not_supported if no SSL/TLS support is available.
*/
int WebTunnelAgent_API webtunnel_agent_configure_tls(bool accept_unknown_cert, bool extended_verification, const char* ciphers, const char* ca_location);


/*
// webtunnel_agent_configure_proxy
//
// Sets up parameters for connecting through a proxy server.
//
// If enable_proxy is true, the connection to the reflector server
// will be attempted through a proxy server.
//
// proxy_host contains the proxy server host name or IP address.
//
// proxy_port contains the port number of the proxy server.
//
// proxy_username contains the username for authenticating against the
// proxy server. If NULL, no authentication will be performed.
//
// proxy_password contains the password for authenticating against the
// proxy server. If NULL, no authentication will be performed.
//
// Returns webtunnel_client_ok if successful, or webtunnel_client_error if an error
// occured.
*/
int WebTunnelAgent_API webtunnel_agent_configure_proxy(bool enable_proxy, const char* proxy_host, unsigned short proxy_port, const char* proxy_username, const char* proxy_password);


/*
// webtunnel_agent_create
//
// Creates a tunnel connection to the reflector service.
//
// remote_uri contains the URI of the remote machine, using the http
// or https URI scheme.
// Example: " https://0a72da53-9de5-44c8-9adf-f3d916304be6.my-devices.net"
//
// username and password are used for authentication against the reflector
// server.
//
// local_addr can be NULL (defaults to 127.0.0.1) or a string containing
// an IP address or host name ("localhost").
//
// Returns NULL in case of an error.
*/
webtunnel_agent WebTunnelAgent_API webtunnel_agent_create(const char* reflector_uri, const char* target_host, const char* device_id, const char* device_password, const char* domain_id, const char* tenant_id, const webtunnel_agent_port_spec* ports, unsigned ports_len, const char* custom_config_path);


int WebTunnelAgent_API webtunnel_agent_get_status(webtunnel_agent wt);


/*
// webtunnel_client_destroy
//
// Closes the given web tunnel connection.
*/
void WebTunnelAgent_API webtunnel_agent_destroy(webtunnel_agent wt);


/*
// webtunnel_agent_last_error_text
//
// Returns a text describing the last encountered error.
// Can be NULL if no descriptive text is available.
*/
const char WebTunnelAgent_API * webtunnel_agent_last_error_text(webtunnel_agent wt);


#ifdef __cplusplus
}
#endif


#endif /* WebTunnelAgent_INCLUDED */
