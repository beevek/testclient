/*
  Copyright 2008-2013 Kristopher R Beevers and Internap Network
  Services Corporation.

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

/*

  Voxel testing client

  Kris Beevers
  kbeevers@voxel.net

  REQUIRES libcurl 7.15.6 or higher.

  This thing takes a file with a list of URLs and then hits those URLs
  in various ways according to some options.

  If a file with md5 sums for each URL is provided, then the hashes
  are checked after downloading is complete for each URL.

  If a file mapping URLs to local files is provided then this program
  will make byte range requests, and compare the md5 sum of the
  downloaded content to the specified byte range in the local copy.

  Optionally, requests can be:
  * terminated mid-download
  * bandwidth throttled

  See the README for more details.

  There are probably some bugs.

 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <vector>
#include <list>
#include <map>
#include <fstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include "options.hpp"

// options
int opt_connections = 80;   // max simultaneous requests to make
bool opt_reuse = false;     // reuse existing connections for new requests
bool opt_random = true;     // select URLs at random, or sequentially?
double opt_br_prob, opt_throttle_prob, opt_term_prob, opt_repeat_prob;
int opt_throttle_min, opt_throttle_max;
double opt_term_min_sec, opt_term_weibull_k, opt_term_weibull_lambda;
bool opt_verbose = false;   // dump vast quantities of debug output on request failure
bool opt_no_checks = false; // no consistency checks, all output > /dev/null
bool opt_quiet = false;
double opt_random_qstring_prob; // prob to add a randomized query string parameter


// input data
std::vector<std::string> url, md5, local, servers, hosts;
std::vector<double> server_weights;
unsigned int url_size, md5_size, local_size;


struct transaction_t
{
  transaction_t();
	~transaction_t();

  CURL *curl;
  curl_slist *headers;
  int url_id;
	char *url_string;
  char outfile_name[128];
  FILE *outfile, *outfile_headers, *outfile_aux;
  char error[CURL_ERROR_SIZE];
  time_t start;
  size_t bytes_sent;
  int byterange_start, byterange_end;
  char byterange_header[128];
	char host_header[128];
  int throttle_bytes_per_sec;
  bool currently_throttling;
  double random_terminate_time;
};

transaction_t::transaction_t()
{
  curl = NULL;
  headers = NULL;
  url_id = -1;
	url_string = 0;
  outfile_name[0] = 0;
  outfile = outfile_headers = outfile_aux = 0;
  error[0] = 0;
  start = 0;
  bytes_sent = 0;
  byterange_start = byterange_end = 0;
  byterange_header[0] = 0;
	host_header[0] = 0;
  throttle_bytes_per_sec = 0;
  currently_throttling = false;
  random_terminate_time = 0.0;
}

transaction_t::~transaction_t()
{
	if(url_string)
		delete [] url_string;
}


CURLM *curl = 0;
std::list<transaction_t> T;
std::map<CURL *, std::list<transaction_t>::iterator> curl_to_T;
int cur_url = 0;
char outfile_extra_name[128];
unsigned int bytes_since_last = 0, bytes = 0;


// print timestamp, then log line, then newline
void mylog(const char *fmt, ...)
{
  char timestamp[100];
  time_t t = time(0);
  struct tm *tmp = localtime(&t);
  strftime(timestamp, 100, "%m/%d/%Y %H:%M:%S", tmp);
  va_list args;
  va_start(args, fmt);
  printf("[%s] ", timestamp);
  vprintf(fmt, args);
  printf("\n");
  va_end(args);
}

size_t discard_data(void *data, size_t sz, size_t nmemb, void *stream)
{
  size_t b = sz * nmemb;
  bytes_since_last += b;
  ((transaction_t *)stream)->bytes_sent += b;
  return b;
}

// assumes weights are normalized
unsigned int weighted_round_robin(const std::vector<double> &weights)
{
	double d = drand48();
	for(unsigned int i = 0; i < weights.size(); d -= weights[i], ++i)
		if(d < 0)
			return i;
	return 0; // should never happen
}

void generate_url(unsigned int url_id, char **url_string)
{
	unsigned int len = url[url_id].length();

	if(!servers.empty())
		len += 7 + 15 + 6; // 'http://' + 'xxx.xxx.xxx.xxx' + ':ppppp' if specified

	static char qstring[14];
	qstring[0] = 0;
	if(opt_random_qstring_prob > 0.0 && drand48() < opt_random_qstring_prob)
		sprintf(qstring, "?q=%d", (unsigned int)(drand48() * 10000000));

	*url_string = new char[len + strlen(qstring) + 1];
	if(servers.empty()) {
		// just use the url as specified in the urls file
		sprintf(*url_string, "%s%s", url[url_id].c_str(), qstring);
	} else {
		// construct a url from the path in urls and a server from the
		// servers file
		unsigned int server_id = weighted_round_robin(server_weights);
		sprintf(*url_string, "http://%s%s%s", servers[server_id].c_str(), url[url_id].c_str(), qstring);
	}
}

void setup_transaction(transaction_t &t)
{
  t.curl = curl_easy_init();
  if(t.curl == NULL) {
    mylog("error: curl_easy_init");
    exit(1);
  }

  if(!opt_no_checks) {
    // dump the content to t.outfile
    if(curl_easy_setopt(t.curl, CURLOPT_WRITEDATA, t.outfile) != CURLE_OK)
      goto setopt_error;
  } else {
    // use our custom output function to discard the output without
    // any slowdowns from writing it to a file descriptor (faster than
    // setting t.outfile to /dev/null)
    if(curl_easy_setopt(t.curl, CURLOPT_WRITEFUNCTION, discard_data) != CURLE_OK)
      goto setopt_error;
    // pass this transaction to discard_data so we can increment byte
    // counts
    if(curl_easy_setopt(t.curl, CURLOPT_WRITEDATA, &t) != CURLE_OK)
      goto setopt_error;
  }

  if(opt_verbose) {
    // dump the headers to t.outfile_headers
    if(curl_easy_setopt(t.curl, CURLOPT_WRITEHEADER, t.outfile_headers) != CURLE_OK)
      goto setopt_error;

    // dump debug output to the aux file
    if(curl_easy_setopt(t.curl, CURLOPT_VERBOSE, 1) != CURLE_OK)
      goto setopt_error;
    if(curl_easy_setopt(t.curl, CURLOPT_STDERR, t.outfile_aux) != CURLE_OK)
      goto setopt_error;
  }

  // 5 sec connection timeout, no transfer timeout
  if(curl_easy_setopt(t.curl, CURLOPT_CONNECTTIMEOUT, 5) != CURLE_OK)
    goto setopt_error;

  // fail on error!
  if(curl_easy_setopt(t.curl, CURLOPT_FAILONERROR, 1) != CURLE_OK)
    goto setopt_error;

  // give me error!
  if(curl_easy_setopt(t.curl, CURLOPT_ERRORBUFFER, t.error) != CURLE_OK)
    goto setopt_error;

  // set the url to hit
	generate_url(t.url_id, &t.url_string);
	if(curl_easy_setopt(t.curl, CURLOPT_URL, t.url_string) != CURLE_OK)
		goto setopt_error;

	// if we have a specific host set for this request, set a Host
	// header
	if(!hosts.empty()) {
		snprintf(t.host_header, 100, "Host: %s", hosts[t.url_id].c_str());
		t.host_header[99] = '\0';
		t.headers = curl_slist_append(t.headers, t.host_header);
	}

  // set byte range header if necessary
  if(t.byterange_end) {
    snprintf(t.byterange_header, 100, "Range: bytes=%d-%d", t.byterange_start, t.byterange_end);
    t.byterange_header[99] = '\0';
    t.headers = curl_slist_append(t.headers, t.byterange_header);
  }

	if(!hosts.empty() || t.byterange_end)
		if(curl_easy_setopt(t.curl, CURLOPT_HTTPHEADER, t.headers) != CURLE_OK)
			goto setopt_error;

  // do not cache dns
  if(curl_easy_setopt(t.curl, CURLOPT_DNS_CACHE_TIMEOUT, 0) != CURLE_OK)
    goto setopt_error;

  if(!opt_reuse) {
    // don't reuse connections for multiple requests
    if(curl_easy_setopt(t.curl, CURLOPT_FORBID_REUSE, 1) != CURLE_OK)
      goto setopt_error;
  }

  return;
 setopt_error:
  mylog("error: curl_easy_setopt");
  exit(1);
}

void md5_compute(FILE *f, int start, int end, std::string &md5)
{
  // initialize openssl md5 digest
  static unsigned char data[102400]; // 100K buffer to read from file
  static unsigned char md_val[EVP_MAX_MD_SIZE];
  unsigned int md_len;
  EVP_MD_CTX mdctx;
  const EVP_MD *md = EVP_md5();
  EVP_MD_CTX_init(&mdctx);
  EVP_DigestInit_ex(&mdctx, md, NULL);

  // read data in chunks from the file and update the digest
  size_t rv;
  if(fseek(f, start, SEEK_SET) != 0) {
    mylog("error: fseek");
    exit(1);
  }
  do {
    rv = end - start + 1;
    if(rv > 102400)
      rv = 102400;
    rv = fread(data, 1, rv, f);
    if(rv == 0)
      break;
    EVP_DigestUpdate(&mdctx, data, rv);
    start += rv;
  } while(start <= end);

  // finalize the digest
  EVP_DigestFinal_ex(&mdctx, md_val, &md_len);
  EVP_MD_CTX_cleanup(&mdctx);

  // output
  md5.reserve(64);
  char digit[4];
  for(unsigned int i = 0; i < md_len; ++i) {
    sprintf(digit, "%02x", md_val[i]);
    md5.append(digit);
  }
}

void write_auxiliary_stats(const transaction_t &t, const char *ip)
{
  char *c;
  int i;
  double d;

  assert(t.outfile_aux);

  curl_easy_getinfo(t.curl, CURLINFO_EFFECTIVE_URL, &c);
  fprintf(t.outfile_aux, "URL: %s\n", c);

  fprintf(t.outfile_aux, "CONNECTED TO: %s\n", ip);

  curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &i);
  fprintf(t.outfile_aux, "RESPONSE CODE: %d\n", i);

  curl_easy_getinfo(t.curl, CURLINFO_TOTAL_TIME, &d);
  fprintf(t.outfile_aux, "TOTAL TIME: %f sec\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_NAMELOOKUP_TIME, &d);
  fprintf(t.outfile_aux, "  DNS: %f sec\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_CONNECT_TIME, &d);
  fprintf(t.outfile_aux, "  CONNECT: %f sec\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_STARTTRANSFER_TIME, &d);
  fprintf(t.outfile_aux, "  FIRST BYTE: %f sec\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_SIZE_UPLOAD, &d);
  fprintf(t.outfile_aux, "TOTAL BYTES UPLOADED: %f\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_SIZE_DOWNLOAD, &d);
  fprintf(t.outfile_aux, "TOTAL BYTES DOWNLOADED: %f\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_SPEED_UPLOAD, &d);
  fprintf(t.outfile_aux, "UPLOAD SPEED: %f Bps\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_SPEED_DOWNLOAD, &d);
  fprintf(t.outfile_aux, "DOWNLOAD SPEED: %f Bps\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &d);
  fprintf(t.outfile_aux, "CONTENT-LENGTH: %f\n", d);

  curl_easy_getinfo(t.curl, CURLINFO_CONTENT_TYPE, &c);
  fprintf(t.outfile_aux, "CONTENT-TYPE: %s\n", c);

  fprintf(t.outfile_aux, "CURL HANDLE ADDRESS: 0x%p\n", (void *)t.curl);
}

void finish_transaction(CURL *handle, int result)
{
  bool noremove = false;
  const char *ip_address = "unknown address";

  assert(handle != NULL);

  // lookup the transaction data, then remove the key from the curl ->
  // T map
  std::map<CURL *, std::list<transaction_t>::iterator>::iterator ctTit =
    curl_to_T.find(handle);
  assert(ctTit != curl_to_T.end());
  std::list<transaction_t>::iterator t = ctTit->second;
  curl_to_T.erase(ctTit);

  assert(t->curl != NULL);
  assert(t != T.end());

  // try to get the ip address we were connected to
  int sock;
  if(curl_easy_getinfo(handle, CURLINFO_LASTSOCKET, &sock) == CURLE_OK) {
    struct sockaddr_in sa;
    socklen_t sa_len = (socklen_t)sizeof(sa);
    if(getpeername(sock, (struct sockaddr *)&sa, &sa_len) == 0)
      ip_address = inet_ntoa(sa.sin_addr);
  }

  if(opt_verbose) {
    // dump some useful stats
    write_auxiliary_stats(*t, ip_address);
  }

  // remove this transaction from the set being serviced by curl
  if(!t->currently_throttling && curl_multi_remove_handle(curl, handle) != CURLM_OK) {
    mylog("error: curl_multi_remove_handle");
    exit(1);
  }
  curl_easy_cleanup(handle);
  if(t->headers)
    curl_slist_free_all(t->headers);

  if(result != 0) { // oops!  an HTTP or connection error!
    mylog("transfer error: %s [%s] --- %s -> %s", url[t->url_id].c_str(), ip_address,
					t->error, t->outfile_name);
    noremove = true;
    goto cleanup;
  }

  // if we're doing no consistency checks, don't try to close any
  // output files since everything is just going to /dev/null anyway
  if(!opt_no_checks)
    noremove = true;

  // consistency checking

  if(!opt_no_checks) {

    fflush(t->outfile);
    struct stat st;
    if(fstat(fileno(t->outfile), &st) < 0)
      mylog("error: fstat on %s", t->outfile);

    if(!t->byterange_end && t->random_terminate_time >= 0 && md5_size == url_size) {
      // full transfer?  if we have md5s, check against that
      std::string xfer_md5;
      md5_compute(t->outfile, 0, st.st_size-1, xfer_md5);
      if(strcmp(xfer_md5.c_str(), md5[t->url_id].c_str())) {
				mylog("full-file md5 error: %s [%s] --- %s (truth) != %s (%d transferred bytes) -> %s",
							url[t->url_id].c_str(), ip_address, md5[t->url_id].c_str(), xfer_md5.c_str(),
							st.st_size, t->outfile_name);
				noremove = true;
				goto cleanup;
      }
    } else if(t->byterange_end && t->random_terminate_time >= 0 && local_size == url_size) {
      // byte range request?  if we have local files, compare the bytes
      std::string xfer_md5, local_md5;
      md5_compute(t->outfile, 0, st.st_size-1, xfer_md5);
      FILE *lf = fopen(local[t->url_id].c_str(), "rb");
      if(!lf) {
				mylog("error: opening %s", local[t->url_id].c_str());
				goto cleanup;
      }

      // first delivery from cache gives the whole file, even if it's
      // a byte range request, so verify appropriately
      if(st.st_size > t->byterange_end - t->byterange_start + 1) {
				struct stat lst;
				if(fstat(fileno(lf), &lst) < 0)
					mylog("error: fstat on %s", local[t->url_id].c_str());

				if(lst.st_size == st.st_size) {
					if(!opt_quiet)
						mylog("first-download cache byte range exception: %s [%s], range %d-%d, got %d bytes",
									url[t->url_id].c_str(), ip_address, t->byterange_start, t->byterange_end, st.st_size);
					local_md5 = md5[t->url_id];
				} else {
					mylog("byte-range size mismatch error: %s [%s] --- %u (truth) != %u (transferred bytes), range %d-%d -> %s",
								url[t->url_id].c_str(), ip_address, lst.st_size, st.st_size,
								t->byterange_start, t->byterange_end, t->outfile_name);
					goto cleanup;
				}
      } else
				md5_compute(lf, t->byterange_start, t->byterange_end, local_md5);

      if(strcmp(xfer_md5.c_str(), local_md5.c_str())) {
				mylog("byte-range md5 error: %s [%s] --- %s (truth) != %s (%d transferred bytes), range %d-%d -> %s",
							url[t->url_id].c_str(), ip_address, local_md5.c_str(), xfer_md5.c_str(), st.st_size,
							t->byterange_start, t->byterange_end, t->outfile_name);
				noremove = true;
				fclose(lf);
				goto cleanup;
      }

      if(lf)
				fclose(lf);
    }

    if(!opt_quiet) {
      if(t->byterange_end)
				mylog("success: %s [%s], range %d-%d --- %d bytes", url[t->url_id].c_str(),
							ip_address, t->byterange_start, t->byterange_end, st.st_size);
      else
				mylog("success: %s [%s] --- %d bytes", url[t->url_id].c_str(), ip_address, st.st_size);
    }

  } // !opt_no_checks


 cleanup:
  if(t != T.end()) {
    if(t->outfile)
      fclose(t->outfile);
    if(t->outfile_headers)
      fclose(t->outfile_headers);
    if(t->outfile_aux)
      fclose(t->outfile_aux);
    if(!noremove) {
      unlink(t->outfile_name);
      if(opt_verbose) {
				strcpy(outfile_extra_name, t->outfile_name);
				strcat(outfile_extra_name, ".header");
				unlink(outfile_extra_name);
				strcpy(outfile_extra_name, t->outfile_name);
				strcat(outfile_extra_name, ".aux");
				unlink(outfile_extra_name);
      }
    }
    T.erase(t);
  }
}

int parse_command_line(int argc, char **argv); // below

void quit(int sig)
{
  mylog("received signal %d, quitting", sig);
  exit(0);
}

int main(int argc, char **argv)
{
  srand48(time(0));
  parse_command_line(argc, argv);

  // initialize curl
  curl = curl_multi_init();

  // turn on curlm pipelining if opt_reuse is set
  if(opt_reuse && curl_multi_setopt(curl, CURLMOPT_PIPELINING, 1) != CURLM_OK) {
    mylog("error: curl_multi_setopt");
    return 1;
  }

  // set some signal handlers; mainly this is useful to exit normally
  // (call "exit") on interruption so that profiler data is written
  // properly for debugging and optimization
  signal(SIGINT, quit);
  signal(SIGQUIT, quit);
  signal(SIGTERM, quit);

  // go go go
  fd_set rfds, wfds;
  int rv, max, running = 0;
  struct timeval tv;
  time_t last_status = 0;
  int prev_url = 0;
  unsigned int done = 0, done_since_last = 0;

  while(1) {

    // maintain the maximum number of simultaneous connections until
    // we're done with all our transactions
    while(T.size() < (unsigned int)opt_connections) {

      std::list<transaction_t>::iterator tit = T.insert(T.end(), transaction_t());
      transaction_t &t = T.back();

      // pick the next URL to hit
      if(opt_repeat_prob && drand48() < opt_repeat_prob) {
				// repeat the previous request
				t.url_id = prev_url;
				if(!opt_quiet)
					mylog("opting to repeat request for %s immediately", url[t.url_id].c_str());
      } else if(opt_random) {
				// choose a random URL
				t.url_id = lrand48() % url_size;
      } else {
				// choose URLs in sequence
				t.url_id = cur_url;
				if((unsigned int)++cur_url >= url_size)
					cur_url = 0;
      }
      prev_url = t.url_id;

      if(!opt_no_checks) {
				// generate a temporary filename to save the data to, then
				// open the content, header, and auxiliary data files
				strcpy(t.outfile_name, "/tmp/testfile.XXXXXX");
				int fd = mkstemp(t.outfile_name);
				t.outfile = fdopen(fd, "w+b");
				if(opt_verbose) {
					strcpy(outfile_extra_name, t.outfile_name);
					strcat(outfile_extra_name, ".header");
					t.outfile_headers = fopen(outfile_extra_name, "w+b");
					strcpy(outfile_extra_name, t.outfile_name);
					strcat(outfile_extra_name, ".aux");
					t.outfile_aux = fopen(outfile_extra_name, "w+b");
				}
				if(!t.outfile || (opt_verbose && (!t.outfile_headers || !t.outfile_aux))) {
					mylog("error: opening %s output set", t.outfile_name);
					return 1;
				}
      }

      // decide whether to make a byte range request
      if(opt_br_prob && drand48() < opt_br_prob) {
				struct stat st;
				if(stat(local[t.url_id].c_str(), &st) < 0)
					mylog("error: stat on %s", local[t.url_id].c_str());
				else {
					// pick random starting/ending bytes
					t.byterange_start = lrand48() % (st.st_size-1);
					t.byterange_end = t.byterange_start + 1 + lrand48() % (st.st_size-1-t.byterange_start);
				}
      }

      // decide whether to terminate randomly, and if so, pick a
      // random wait time after which we'll terminate
      if(opt_term_prob && drand48() < opt_term_prob) {
				t.random_terminate_time = opt_term_min_sec +
					pow(opt_term_weibull_lambda*(-log(drand48())), 1.0/opt_term_weibull_k);
      }

      // decide whether (and how much) to throttle the connection
      if(opt_throttle_prob && drand48() < opt_throttle_prob)
				t.throttle_bytes_per_sec = opt_throttle_min + lrand48() % (opt_throttle_max-opt_throttle_min);

      t.start = time(0);

      // add the transaction
      setup_transaction(t);
      if(curl_multi_add_handle(curl, t.curl) != CURLM_OK) {
				mylog("error: curl_multi_add_handle");
				return 1;
      }

      // update the curl -> T map
      curl_to_T[t.curl] = tit;
    }

    // select on current transactions
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    max = -1;
    if(curl_multi_fdset(curl, &rfds, &wfds, 0, &max) != CURLM_OK) {
      mylog("error: curl_multi_fdset");
      return 1;
    }
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if(max < 0) max = 0;
    rv = select(max + 1, &rfds, &wfds, 0, &tv);
    if(rv < 0) {
      if(errno == EINTR)
				continue;
      mylog("error: select (%d)", errno);
      return 1;
    }

    int total_transactions = T.size();

    // run curl on transactions with data waiting
    do {
      rv = curl_multi_perform(curl, &running);
      if(rv != CURLM_OK && rv != CURLM_CALL_MULTI_PERFORM) {
				mylog("error: curl_multi_perform");
				return 1;
      }
    } while(rv == CURLM_CALL_MULTI_PERFORM);

    // clean up completed transactions and do various tests
    struct CURLMsg *msg;
    while((msg = curl_multi_info_read(curl, &rv))) {
      if(msg == NULL) {
				mylog("error: curl_multi_info_read");
				return 1;
      }
      if(msg->msg != CURLMSG_DONE)
				continue;
      finish_transaction(msg->easy_handle, msg->data.result);
      ++done_since_last;
    }

    // go through the transactions and simulate some stuff; skip this
    // if none of the simulations are enabled since just the looping
    // takes up a fair amount of CPU
    int throttling = 0;
    time_t now = time(0);
    std::list<transaction_t>::iterator t = T.begin(), tmp;

    if(opt_term_prob > 0 || opt_throttle_prob > 0) {
      for(; t != T.end(); ++t) {
      simulate_top:
				// should we terminate this transaction early?
				if(t->random_terminate_time && double(now - t->start) > t->random_terminate_time) {
					if(!opt_quiet)
						mylog("terminating request for %s after %d seconds", url[t->url_id].c_str(), now - t->start);
					t->random_terminate_time = -1.0; // to notify finish_transaction
					tmp = t;
					++t;
					finish_transaction(tmp->curl, 0);
					++done_since_last;
					if(t == T.end())
						break;
					goto simulate_top;
				}

				// throttle, if necessary, by temporarily removing the
				// transaction from the curl multi handle; if we are currently
				// throttling, decide whether to reinstate the transaction
				if(t->throttle_bytes_per_sec) {
					if(!opt_no_checks) {
						// compute bytes_sent from the file since we don't use a
						// custom write function if we're saving the data
						struct stat st;
						fstat(fileno(t->outfile), &st);
						t->bytes_sent = st.st_size;
					}
					double Bps = double(t->bytes_sent) / double(now - t->start);

					if(!t->currently_throttling && Bps > t->throttle_bytes_per_sec) {
						t->currently_throttling = true;
						if(curl_multi_remove_handle(curl, t->curl) != CURLM_OK) {
							mylog("error: murl_multi_remove_handle");
							return 1;
						}
					} else if(t->currently_throttling && Bps <= t->throttle_bytes_per_sec) {
						t->currently_throttling = false;
						if(curl_multi_add_handle(curl, t->curl) != CURLM_OK) {
							mylog("error: curl_multi_add_handle");
							return 1;
						}
					}

					if(t->currently_throttling)
						++throttling;
				}
      }
    }

    // print out status once per second
    if(now - last_status > 0) {
      done += done_since_last;
      bytes += bytes_since_last;
      if(opt_no_checks)
				mylog("status: %d transfers, %d finished, %d throttling, ~%d req per sec, ~%d Bps download",
							total_transactions, done, throttling, done_since_last, bytes_since_last);
      else
				mylog("status: %d transfers, %d finished, %d throttling, ~%d req per sec",
							total_transactions, done, throttling, done_since_last);
      last_status = now;
      done_since_last = 0;
      bytes_since_last = 0;
    }

  }

  curl_multi_cleanup(curl);

  return 0;
}


// assumes lines are < 1024 characters
int file_to_string_vector(const char *file, std::vector<std::string> &lines)
{
  FILE *f = fopen(file, "r");
  if(!f)
    return 1;
  char line[1024], *nl;
  while(fgets(line, 1024, f)) {
    nl = strchr(line, '\n');
    if(nl)
      *nl = '\0'; // truncate
		if(strlen(line))
			lines.push_back(std::string(line));
  }
  fclose(f);
  return 0;
}

int parse_command_line(int argc, char **argv)
{
  // set up commandline/configuration file options
  options::add<bool>("help", 0, "Print usage information", 0, false, options::nodump);
  options::set_cf_options("config", "c");
  options::add<std::string>("save-config", 0, "Save configuration file", 0, "", options::nodump);

  options::add<std::string>("md5-list", "m", "File with MD5 sums for each URL", "Input", "");
  options::add<std::string>("local-list", "l", "File with local filenames for each URL", "Input", "");
	options::add<std::string>("server-list", 0, "File with server IPs and weights", "Input", "");

  options::add<int>("num-transactions", "n", "Number of simultaneous transactions to maintain",
										"Traffic simulation", 80);
  options::add<bool>("reuse-connections", "u", "Keep connections open and reuse them for new requests",
										 "Traffic simulation", false);
  options::add<bool>("random", "r", "Request URLs in random order (default)", "Traffic simulation", true);
  options::add<bool>("sequential", "s", "Request URLs in sequential order", "Traffic simulation", false);
	options::add<double>("random-qstring-prob", 0, "Probability of adding a random query string parameter to the URL",
											 "Traffic simulation", 0.0);
  options::add<double>("br-prob", "b", "Probability of making a byte range request (requires local-list)",
											 "Traffic simulation", 0.0);
  options::add<double>("throttle-prob", "o", "Probability of throttling connection speed for a request",
											 "Traffic simulation", 0.0);
  options::add<int>("throttle-min", "i", "Randomized throttling: minimum bytes/sec",
										"Traffic simulation", 10000000);
  options::add<int>("throttle-max", "a", "Randomized throttling: maximum bytes/sec",
										"Traffic simulation", 10000000);
  options::add<double>("term-prob", "t", "Probability of considering early termination for a request",
											 "Traffic simulation", 0.0);
  options::add<double>("term-min-sec", "e", "Seconds before we start considering early termination",
											 "Traffic simulation", 100000000000.0);
  options::add<double>("term-weibull-k", "k", "Weibull PDF k parameter", "Traffic simulation", 1.2);
  options::add<double>("term-weibull-lambda", "d", "Weibull PDF lambda parameter",
											 "Traffic simulation", 30.0);
  options::add<double>("repeat-prob", "p", "Probability of the previous request being repeated immediately",
											 "Traffic simulation", 0.0);

  options::add<bool>("verbose", "v", "Dump lots of debug output on request failure",
										 "Output", false);
  options::add<bool>("no-checks", "x", "Don't do any consistency checking; dump content to /dev/null",
										 "Output", false);
  options::add<bool>("quiet", "q", "Quiet: log only status information, errors, and nothing else",
										 "Output", false);

  int inpidx = options::parse_cmdline(argc, argv);

  if(inpidx < 0) // some kind of error
    exit(1);    // relevant info printed by getopt

  // print usage information
  if(inpidx >= argc || options::quickget<bool>("help") == true) {
    std::cerr << "Usage: " << argv[0] << " [options] url-file" << std::endl;
    options::print_options(std::cout);
    exit(1);
  }

  // save a config file based on these options?
  std::string cfname = options::quickget<std::string>("save-config");
  if(cfname.length() > 0) {
    std::ofstream conf(cfname.c_str());
    if(!conf)
      std::cerr << "Can't write configuration file " << cfname << std::endl;
    else
      options::dump(conf);
  }

  // read in URL list
  if(file_to_string_vector(argv[inpidx], url) < 0) {
    mylog("Can't read in %s", argv[inpidx]);
    exit(1);
  }

  // read in MD5 list
  if(options::quickget<std::string>("md5-list").length()) {
    if(file_to_string_vector(options::quickget<std::string>("md5-list").c_str(), md5) < 0) {
      mylog("Can't read in %s", options::quickget<std::string>("md5-list").c_str());
      exit(1);
    }
    if(md5.size() != url.size()) {
      mylog("MD5 list must be same size as URL list");
      exit(1);
    }
  }

  // read in local file list
  if(options::quickget<std::string>("local-list").length()) {
    if(file_to_string_vector(options::quickget<std::string>("local-list").c_str(), local) < 0) {
      mylog("Can't read in %s", options::quickget<std::string>("local-list").c_str());
      exit(1);
    }
    if(local.size() != url.size()) {
      mylog("Local file list must be same size as URL list");
      exit(1);
    }
  }

	if(options::quickget<std::string>("server-list").length()) {
    if(file_to_string_vector(options::quickget<std::string>("server-list").c_str(), servers) < 0) {
      mylog("Can't read in %s", options::quickget<std::string>("server-list").c_str());
      exit(1);
    }

		// parse out the weights
		double W = 0.0, w;
		unsigned int i;
		for(i = 0; i < servers.size(); ++i) {
			size_t sp = servers[i].find_first_of(" \t");
			std::string ss(sp != servers[i].npos ? servers[i].substr(sp) : "");
			if(sp == servers[i].npos || (w = atof(ss.substr(ss.find_first_not_of(" \t")).c_str())) == 0.0)
				server_weights.push_back(1.0);
			else {
				server_weights.push_back(w);
				servers[i].erase(sp);
			}
			W += server_weights[i];
		}
		for(i = 0; i < server_weights.size(); ++i)
			server_weights[i] /= W; // normalize weights

		// we've got servers, convert urls into paths and put the host
		// names in a separate vector
		for(i = 0; i < url.size(); ++i) {
			url[i].erase(0, 7); // assume all the urls start with 'http://'
			size_t sl = url[i].find_first_of("/");
			hosts.push_back(url[i].substr(0, sl));
			url[i].erase(0, sl);
		}
	}

  url_size = url.size();
  md5_size = md5.size();
  local_size = local.size();

  opt_reuse = options::quickget<bool>("reuse-connections");
  opt_random = !options::quickget<bool>("sequential");
  opt_connections = options::quickget<int>("num-transactions");
  opt_br_prob = local_size == url_size ? options::quickget<double>("br-prob") : 0.0;
  opt_throttle_prob = options::quickget<double>("throttle-prob");
  opt_throttle_min = options::quickget<int>("throttle-min");
  opt_throttle_max = options::quickget<int>("throttle-max");
  opt_term_prob = options::quickget<double>("term-prob");
  opt_term_min_sec = options::quickget<double>("term-min-sec");
  opt_term_weibull_k = options::quickget<double>("term-min-weibull-k");
  opt_term_weibull_lambda = options::quickget<double>("term-min-weibull-lambda");
  opt_repeat_prob = options::quickget<double>("repeat-prob");
  opt_verbose = options::quickget<bool>("verbose");
  opt_no_checks = options::quickget<bool>("no-checks");
  opt_quiet = options::quickget<bool>("quiet");
	opt_random_qstring_prob = options::quickget<double>("random-qstring-prob");

  if(opt_no_checks)
    opt_verbose = false;

  return 0;
}
