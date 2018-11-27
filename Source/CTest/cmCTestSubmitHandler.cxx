/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmCTestSubmitHandler.h"

#include "cm_curl.h"
#include "cm_jsoncpp_reader.h"
#include "cm_jsoncpp_value.h"
#include <chrono>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>

#include "cmAlgorithms.h"
#include "cmCTest.h"
#include "cmCTestCurl.h"
#include "cmCTestScriptHandler.h"
#include "cmCryptoHash.h"
#include "cmCurl.h"
#include "cmDuration.h"
#include "cmGeneratedFileStream.h"
#include "cmState.h"
#include "cmSystemTools.h"
#include "cmXMLParser.h"
#include "cmake.h"

#define SUBMIT_TIMEOUT_IN_SECONDS_DEFAULT 120

typedef std::vector<char> cmCTestSubmitHandlerVectorOfChar;

class cmCTestSubmitHandler::ResponseParser : public cmXMLParser
{
public:
  ResponseParser() { this->Status = STATUS_OK; }
  ~ResponseParser() override {}

public:
  enum StatusType
  {
    STATUS_OK,
    STATUS_WARNING,
    STATUS_ERROR
  };

  StatusType Status;
  std::string Filename;
  std::string MD5;
  std::string Message;
  std::string BuildID;

private:
  std::vector<char> CurrentValue;

  std::string GetCurrentValue()
  {
    std::string val;
    if (!this->CurrentValue.empty()) {
      val.assign(&this->CurrentValue[0], this->CurrentValue.size());
    }
    return val;
  }

  void StartElement(const std::string& /*name*/,
                    const char** /*atts*/) override
  {
    this->CurrentValue.clear();
  }

  void CharacterDataHandler(const char* data, int length) override
  {
    this->CurrentValue.insert(this->CurrentValue.end(), data, data + length);
  }

  void EndElement(const std::string& name) override
  {
    if (name == "status") {
      std::string status = cmSystemTools::UpperCase(this->GetCurrentValue());
      if (status == "OK" || status == "SUCCESS") {
        this->Status = STATUS_OK;
      } else if (status == "WARNING") {
        this->Status = STATUS_WARNING;
      } else {
        this->Status = STATUS_ERROR;
      }
    } else if (name == "filename") {
      this->Filename = this->GetCurrentValue();
    } else if (name == "md5") {
      this->MD5 = this->GetCurrentValue();
    } else if (name == "message") {
      this->Message = this->GetCurrentValue();
    } else if (name == "buildId") {
      this->BuildID = this->GetCurrentValue();
    }
  }
};

static size_t cmCTestSubmitHandlerWriteMemoryCallback(void* ptr, size_t size,
                                                      size_t nmemb, void* data)
{
  int realsize = static_cast<int>(size * nmemb);

  cmCTestSubmitHandlerVectorOfChar* vec =
    static_cast<cmCTestSubmitHandlerVectorOfChar*>(data);
  const char* chPtr = static_cast<char*>(ptr);
  vec->insert(vec->end(), chPtr, chPtr + realsize);

  return realsize;
}

static size_t cmCTestSubmitHandlerCurlDebugCallback(CURL* /*unused*/,
                                                    curl_infotype /*unused*/,
                                                    char* chPtr, size_t size,
                                                    void* data)
{
  cmCTestSubmitHandlerVectorOfChar* vec =
    static_cast<cmCTestSubmitHandlerVectorOfChar*>(data);
  vec->insert(vec->end(), chPtr, chPtr + size);

  return size;
}

cmCTestSubmitHandler::cmCTestSubmitHandler()
  : HTTPProxy()
  , FTPProxy()
{
  this->Initialize();
}

void cmCTestSubmitHandler::Initialize()
{
  // We submit all available parts by default.
  for (cmCTest::Part p = cmCTest::PartStart; p != cmCTest::PartCount;
       p = cmCTest::Part(p + 1)) {
    this->SubmitPart[p] = true;
  }
  this->CDash = false;
  this->HasWarnings = false;
  this->HasErrors = false;
  this->Superclass::Initialize();
  this->HTTPProxy.clear();
  this->HTTPProxyType = 0;
  this->HTTPProxyAuth.clear();
  this->FTPProxy.clear();
  this->FTPProxyType = 0;
  this->LogFile = nullptr;
  this->Files.clear();
}

bool cmCTestSubmitHandler::SubmitUsingFTP(
  const std::string& localprefix, const std::vector<std::string>& files,
  const std::string& remoteprefix, const std::string& url)
{
  CURL* curl;
  CURLcode res;
  FILE* ftpfile;
  char error_buffer[1024];

  /* In windows, this will init the winsock stuff */
  ::curl_global_init(CURL_GLOBAL_ALL);

  for (std::string const& file : files) {
    /* get a curl handle */
    curl = curl_easy_init();
    if (curl) {
      // Using proxy
      if (this->FTPProxyType > 0) {
        curl_easy_setopt(curl, CURLOPT_PROXY, this->FTPProxy.c_str());
        switch (this->FTPProxyType) {
          case 2:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            break;
          case 3:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            break;
          default:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        }
      }

      // enable uploading
      ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);

      // if there is little to no activity for too long stop submitting
      ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
      ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         SUBMIT_TIMEOUT_IN_SECONDS_DEFAULT);

      ::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);

      std::string local_file = file;
      if (!cmSystemTools::FileExists(local_file)) {
        local_file = localprefix + "/" + file;
      }
      std::string upload_as =
        url + "/" + remoteprefix + cmSystemTools::GetFilenameName(file);

      if (!cmSystemTools::FileExists(local_file)) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Cannot find file: " << local_file << std::endl);
        ::curl_easy_cleanup(curl);
        ::curl_global_cleanup();
        return false;
      }
      unsigned long filelen = cmSystemTools::FileLength(local_file);

      ftpfile = cmsys::SystemTools::Fopen(local_file, "rb");
      *this->LogFile << "\tUpload file: " << local_file << " to " << upload_as
                     << std::endl;
      cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                         "   Upload file: " << local_file << " to "
                                            << upload_as << std::endl,
                         this->Quiet);

      ::curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

      // specify target
      ::curl_easy_setopt(curl, CURLOPT_URL, upload_as.c_str());

      // now specify which file to upload
      ::curl_easy_setopt(curl, CURLOPT_INFILE, ftpfile);

      // and give the size of the upload (optional)
      ::curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(filelen));

      // and give curl the buffer for errors
      ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer);

      // specify handler for output
      ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         cmCTestSubmitHandlerWriteMemoryCallback);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
                         cmCTestSubmitHandlerCurlDebugCallback);

      /* we pass our 'chunk' struct to the callback function */
      cmCTestSubmitHandlerVectorOfChar chunk;
      cmCTestSubmitHandlerVectorOfChar chunkDebug;
      ::curl_easy_setopt(curl, CURLOPT_FILE, &chunk);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &chunkDebug);

      // Now run off and do what you've been told!
      res = ::curl_easy_perform(curl);

      if (!chunk.empty()) {
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "CURL output: ["
                             << cmCTestLogWrite(&*chunk.begin(), chunk.size())
                             << "]" << std::endl,
                           this->Quiet);
      }
      if (!chunkDebug.empty()) {
        cmCTestOptionalLog(
          this->CTest, DEBUG,
          "CURL debug output: ["
            << cmCTestLogWrite(&*chunkDebug.begin(), chunkDebug.size()) << "]"
            << std::endl,
          this->Quiet);
      }

      fclose(ftpfile);
      if (res) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error when uploading file: " << local_file
                                                    << std::endl);
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error message was: " << error_buffer << std::endl);
        *this->LogFile << "   Error when uploading file: " << local_file
                       << std::endl
                       << "   Error message was: " << error_buffer << std::endl
                       << "   Curl output was: ";
        // avoid dereference of empty vector
        if (!chunk.empty()) {
          *this->LogFile << cmCTestLogWrite(&*chunk.begin(), chunk.size());
          cmCTestLog(this->CTest, ERROR_MESSAGE,
                     "CURL output: ["
                       << cmCTestLogWrite(&*chunk.begin(), chunk.size()) << "]"
                       << std::endl);
        }
        *this->LogFile << std::endl;
        ::curl_easy_cleanup(curl);
        ::curl_global_cleanup();
        return false;
      }
      // always cleanup
      ::curl_easy_cleanup(curl);
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Uploaded: " + local_file << std::endl,
                         this->Quiet);
    }
  }
  ::curl_global_cleanup();
  return true;
}

// Uploading files is simpler
bool cmCTestSubmitHandler::SubmitUsingHTTP(
  const std::string& localprefix, const std::vector<std::string>& files,
  const std::string& remoteprefix, const std::string& url)
{
  CURL* curl;
  CURLcode res;
  FILE* ftpfile;
  char error_buffer[1024];
  // Set Content-Type to satisfy fussy modsecurity rules.
  struct curl_slist* headers =
    ::curl_slist_append(nullptr, "Content-Type: text/xml");

  // Add any additional headers that the user specified.
  for (std::string const& h : this->HttpHeaders) {
    cmCTestOptionalLog(this->CTest, DEBUG,
                       "   Add HTTP Header: \"" << h << "\"" << std::endl,
                       this->Quiet);
    headers = ::curl_slist_append(headers, h.c_str());
  }

  /* In windows, this will init the winsock stuff */
  ::curl_global_init(CURL_GLOBAL_ALL);
  std::string dropMethod(this->CTest->GetCTestConfiguration("DropMethod"));
  std::string curlopt(this->CTest->GetCTestConfiguration("CurlOptions"));
  std::vector<std::string> args;
  cmSystemTools::ExpandListArgument(curlopt, args);
  bool verifyPeerOff = false;
  bool verifyHostOff = false;
  for (std::string const& arg : args) {
    if (arg == "CURLOPT_SSL_VERIFYPEER_OFF") {
      verifyPeerOff = true;
    }
    if (arg == "CURLOPT_SSL_VERIFYHOST_OFF") {
      verifyHostOff = true;
    }
  }
  for (std::string const& file : files) {
    /* get a curl handle */
    curl = curl_easy_init();
    if (curl) {
      cmCurlSetCAInfo(curl);
      if (verifyPeerOff) {
        cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                           "  Set CURLOPT_SSL_VERIFYPEER to off\n",
                           this->Quiet);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
      }
      if (verifyHostOff) {
        cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                           "  Set CURLOPT_SSL_VERIFYHOST to off\n",
                           this->Quiet);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
      }

      // Using proxy
      if (this->HTTPProxyType > 0) {
        curl_easy_setopt(curl, CURLOPT_PROXY, this->HTTPProxy.c_str());
        switch (this->HTTPProxyType) {
          case 2:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            break;
          case 3:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            break;
          default:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            if (!this->HTTPProxyAuth.empty()) {
              curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD,
                               this->HTTPProxyAuth.c_str());
            }
        }
      }
      if (this->CTest->ShouldUseHTTP10()) {
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
      }
      // enable HTTP ERROR parsing
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
      /* enable uploading */
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);

      // if there is little to no activity for too long stop submitting
      ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1);
      ::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                         SUBMIT_TIMEOUT_IN_SECONDS_DEFAULT);

      /* HTTP PUT please */
      ::curl_easy_setopt(curl, CURLOPT_PUT, 1);
      ::curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

      ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      std::string local_file = file;
      bool initialize_cdash_buildid = false;
      if (!cmSystemTools::FileExists(local_file)) {
        local_file = localprefix + "/" + file;
        // If this file exists within the local Testing directory we assume
        // that it will be associated with the current build in CDash.
        initialize_cdash_buildid = true;
      }
      std::string remote_file =
        remoteprefix + cmSystemTools::GetFilenameName(file);

      *this->LogFile << "\tUpload file: " << local_file << " to "
                     << remote_file << std::endl;

      std::string ofile;
      for (char c : remote_file) {
        char hexCh[4] = { 0, 0, 0, 0 };
        hexCh[0] = c;
        switch (c) {
          case '+':
          case '?':
          case '/':
          case '\\':
          case '&':
          case ' ':
          case '=':
          case '%':
            sprintf(hexCh, "%%%02X", static_cast<int>(c));
            ofile.append(hexCh);
            break;
          default:
            ofile.append(hexCh);
        }
      }
      std::string upload_as = url +
        ((url.find('?') == std::string::npos) ? '?' : '&') +
        "FileName=" + ofile;

      if (initialize_cdash_buildid) {
        // Provide extra arguments to CDash so that it can initialize and
        // return a buildid.
        cmCTestCurl ctest_curl(this->CTest);
        upload_as += "&build=";
        upload_as +=
          ctest_curl.Escape(this->CTest->GetCTestConfiguration("BuildName"));
        upload_as += "&site=";
        upload_as +=
          ctest_curl.Escape(this->CTest->GetCTestConfiguration("Site"));
        upload_as += "&stamp=";
        upload_as += ctest_curl.Escape(this->CTest->GetCurrentTag());
        upload_as += "-";
        upload_as += ctest_curl.Escape(this->CTest->GetTestModelString());
        cmCTestScriptHandler* ch = static_cast<cmCTestScriptHandler*>(
          this->CTest->GetHandler("script"));
        cmake* cm = ch->GetCMake();
        if (cm) {
          const char* subproject =
            cm->GetState()->GetGlobalProperty("SubProject");
          if (subproject) {
            upload_as += "&subproject=";
            upload_as += ctest_curl.Escape(subproject);
          }
        }
      }

      upload_as += "&MD5=";

      if (cmSystemTools::IsOn(this->GetOption("InternalTest"))) {
        upload_as += "bad_md5sum";
      } else {
        upload_as +=
          cmSystemTools::ComputeFileHash(local_file, cmCryptoHash::AlgoMD5);
      }

      // Generate Done.xml right before it is submitted.
      // The reason for this is two-fold:
      // 1) It must be generated after some other part has been submitted
      //    so we have a buildId to refer to in its contents.
      // 2) By generating Done.xml here its timestamp will be as late as
      //    possible. This gives us a more accurate record of how long the
      //    entire build took to complete.
      if (file == "Done.xml") {
        this->CTest->GenerateDoneFile();
      }

      if (!cmSystemTools::FileExists(local_file)) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Cannot find file: " << local_file << std::endl);
        ::curl_easy_cleanup(curl);
        ::curl_slist_free_all(headers);
        ::curl_global_cleanup();
        return false;
      }
      unsigned long filelen = cmSystemTools::FileLength(local_file);

      ftpfile = cmsys::SystemTools::Fopen(local_file, "rb");
      cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                         "   Upload file: " << local_file << " to "
                                            << upload_as << " Size: "
                                            << filelen << std::endl,
                         this->Quiet);

      // specify target
      ::curl_easy_setopt(curl, CURLOPT_URL, upload_as.c_str());

      // CURLAUTH_BASIC is default, and here we allow additional methods,
      // including more secure ones
      ::curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);

      // now specify which file to upload
      ::curl_easy_setopt(curl, CURLOPT_INFILE, ftpfile);

      // and give the size of the upload (optional)
      ::curl_easy_setopt(curl, CURLOPT_INFILESIZE, static_cast<long>(filelen));

      // and give curl the buffer for errors
      ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer);

      // specify handler for output
      ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         cmCTestSubmitHandlerWriteMemoryCallback);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
                         cmCTestSubmitHandlerCurlDebugCallback);

      /* we pass our 'chunk' struct to the callback function */
      cmCTestSubmitHandlerVectorOfChar chunk;
      cmCTestSubmitHandlerVectorOfChar chunkDebug;
      ::curl_easy_setopt(curl, CURLOPT_FILE, &chunk);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &chunkDebug);

      // Now run off and do what you've been told!
      res = ::curl_easy_perform(curl);

      if (!chunk.empty()) {
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "CURL output: ["
                             << cmCTestLogWrite(&*chunk.begin(), chunk.size())
                             << "]" << std::endl,
                           this->Quiet);
        this->ParseResponse(chunk);
      }
      if (!chunkDebug.empty()) {
        cmCTestOptionalLog(
          this->CTest, DEBUG,
          "CURL debug output: ["
            << cmCTestLogWrite(&*chunkDebug.begin(), chunkDebug.size()) << "]"
            << std::endl,
          this->Quiet);
      }

      // If curl failed for any reason, or checksum fails, wait and retry
      //
      if (res != CURLE_OK || this->HasErrors) {
        std::string retryDelay = this->GetOption("RetryDelay") == nullptr
          ? ""
          : this->GetOption("RetryDelay");
        std::string retryCount = this->GetOption("RetryCount") == nullptr
          ? ""
          : this->GetOption("RetryCount");

        auto delay = cmDuration(
          retryDelay.empty()
            ? atoi(this->CTest->GetCTestConfiguration("CTestSubmitRetryDelay")
                     .c_str())
            : atoi(retryDelay.c_str()));
        int count = retryCount.empty()
          ? atoi(this->CTest->GetCTestConfiguration("CTestSubmitRetryCount")
                   .c_str())
          : atoi(retryCount.c_str());

        for (int i = 0; i < count; i++) {
          cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                             "   Submit failed, waiting " << delay.count()
                                                          << " seconds...\n",
                             this->Quiet);

          auto stop = std::chrono::steady_clock::now() + delay;
          while (std::chrono::steady_clock::now() < stop) {
            cmSystemTools::Delay(100);
          }

          cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                             "   Retry submission: Attempt "
                               << (i + 1) << " of " << count << std::endl,
                             this->Quiet);

          ::fclose(ftpfile);
          ftpfile = cmsys::SystemTools::Fopen(local_file, "rb");
          ::curl_easy_setopt(curl, CURLOPT_INFILE, ftpfile);

          chunk.clear();
          chunkDebug.clear();
          this->HasErrors = false;

          res = ::curl_easy_perform(curl);

          if (!chunk.empty()) {
            cmCTestOptionalLog(
              this->CTest, DEBUG,
              "CURL output: ["
                << cmCTestLogWrite(&*chunk.begin(), chunk.size()) << "]"
                << std::endl,
              this->Quiet);
            this->ParseResponse(chunk);
          }

          if (res == CURLE_OK && !this->HasErrors) {
            break;
          }
        }
      }

      fclose(ftpfile);
      if (res) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error when uploading file: " << local_file
                                                    << std::endl);
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error message was: " << error_buffer << std::endl);
        *this->LogFile << "   Error when uploading file: " << local_file
                       << std::endl
                       << "   Error message was: " << error_buffer
                       << std::endl;
        // avoid deref of begin for zero size array
        if (!chunk.empty()) {
          *this->LogFile << "   Curl output was: "
                         << cmCTestLogWrite(&*chunk.begin(), chunk.size())
                         << std::endl;
          cmCTestLog(this->CTest, ERROR_MESSAGE,
                     "CURL output: ["
                       << cmCTestLogWrite(&*chunk.begin(), chunk.size()) << "]"
                       << std::endl);
        }
        ::curl_easy_cleanup(curl);
        ::curl_slist_free_all(headers);
        ::curl_global_cleanup();
        return false;
      }
      // always cleanup
      ::curl_easy_cleanup(curl);
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Uploaded: " + local_file << std::endl,
                         this->Quiet);
    }
  }
  ::curl_slist_free_all(headers);
  ::curl_global_cleanup();
  return true;
}

void cmCTestSubmitHandler::ParseResponse(
  cmCTestSubmitHandlerVectorOfChar chunk)
{
  std::string output;
  output.append(chunk.begin(), chunk.end());

  if (output.find("<cdash") != std::string::npos) {
    ResponseParser parser;
    parser.Parse(output.c_str());

    if (parser.Status != ResponseParser::STATUS_OK) {
      this->HasErrors = true;
      cmCTestLog(this->CTest, HANDLER_OUTPUT,
                 "   Submission failed: " << parser.Message << std::endl);
      return;
    }
    this->CTest->SetBuildID(parser.BuildID);
  }
  output = cmSystemTools::UpperCase(output);
  if (output.find("WARNING") != std::string::npos) {
    this->HasWarnings = true;
  }
  if (output.find("ERROR") != std::string::npos) {
    this->HasErrors = true;
  }

  if (this->HasWarnings || this->HasErrors) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT,
               "   Server Response:\n"
                 << cmCTestLogWrite(&*chunk.begin(), chunk.size()) << "\n");
  }
}

bool cmCTestSubmitHandler::TriggerUsingHTTP(
  const std::vector<std::string>& files, const std::string& remoteprefix,
  const std::string& url)
{
  CURL* curl;
  char error_buffer[1024];

  /* In windows, this will init the winsock stuff */
  ::curl_global_init(CURL_GLOBAL_ALL);

  for (std::string const& file : files) {
    /* get a curl handle */
    curl = curl_easy_init();
    if (curl) {
      // Using proxy
      if (this->HTTPProxyType > 0) {
        curl_easy_setopt(curl, CURLOPT_PROXY, this->HTTPProxy.c_str());
        switch (this->HTTPProxyType) {
          case 2:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4);
            break;
          case 3:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
            break;
          default:
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
            if (!this->HTTPProxyAuth.empty()) {
              curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD,
                               this->HTTPProxyAuth.c_str());
            }
        }
      }

      ::curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

      // and give curl the buffer for errors
      ::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer);

      // specify handler for output
      ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         cmCTestSubmitHandlerWriteMemoryCallback);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
                         cmCTestSubmitHandlerCurlDebugCallback);

      /* we pass our 'chunk' struct to the callback function */
      cmCTestSubmitHandlerVectorOfChar chunk;
      cmCTestSubmitHandlerVectorOfChar chunkDebug;
      ::curl_easy_setopt(curl, CURLOPT_FILE, &chunk);
      ::curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &chunkDebug);

      std::string rfile = remoteprefix + cmSystemTools::GetFilenameName(file);
      std::string ofile;
      for (char c : rfile) {
        char hexCh[4] = { 0, 0, 0, 0 };
        hexCh[0] = c;
        switch (c) {
          case '+':
          case '?':
          case '/':
          case '\\':
          case '&':
          case ' ':
          case '=':
          case '%':
            sprintf(hexCh, "%%%02X", static_cast<int>(c));
            ofile.append(hexCh);
            break;
          default:
            ofile.append(hexCh);
        }
      }
      std::string turl = url +
        ((url.find('?') == std::string::npos) ? '?' : '&') +
        "xmlfile=" + ofile;
      *this->LogFile << "Trigger url: " << turl << std::endl;
      cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                         "   Trigger url: " << turl << std::endl, this->Quiet);
      curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
      curl_easy_setopt(curl, CURLOPT_URL, turl.c_str());
      if (curl_easy_perform(curl)) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error when triggering: " << turl << std::endl);
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Error message was: " << error_buffer << std::endl);
        *this->LogFile << "\tTriggering failed with error: " << error_buffer
                       << std::endl
                       << "   Error message was: " << error_buffer
                       << std::endl;
        if (!chunk.empty()) {
          *this->LogFile << "   Curl output was: "
                         << cmCTestLogWrite(&*chunk.begin(), chunk.size())
                         << std::endl;
          cmCTestLog(this->CTest, ERROR_MESSAGE,
                     "CURL output: ["
                       << cmCTestLogWrite(&*chunk.begin(), chunk.size()) << "]"
                       << std::endl);
        }
        ::curl_easy_cleanup(curl);
        ::curl_global_cleanup();
        return false;
      }

      if (!chunk.empty()) {
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "CURL output: ["
                             << cmCTestLogWrite(&*chunk.begin(), chunk.size())
                             << "]" << std::endl,
                           this->Quiet);
      }
      if (!chunkDebug.empty()) {
        cmCTestOptionalLog(
          this->CTest, DEBUG,
          "CURL debug output: ["
            << cmCTestLogWrite(&*chunkDebug.begin(), chunkDebug.size()) << "]"
            << std::endl,
          this->Quiet);
      }

      // always cleanup
      ::curl_easy_cleanup(curl);
      cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT, std::endl,
                         this->Quiet);
    }
  }
  ::curl_global_cleanup();
  cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                     "   Dart server triggered..." << std::endl, this->Quiet);
  return true;
}

void cmCTestSubmitHandler::ConstructCDashURL(std::string& dropMethod,
                                             std::string& url)
{
  dropMethod = this->CTest->GetCTestConfiguration("DropMethod");
  url = dropMethod;
  url += "://";
  if (!this->CTest->GetCTestConfiguration("DropSiteUser").empty()) {
    url += this->CTest->GetCTestConfiguration("DropSiteUser");
    cmCTestOptionalLog(
      this->CTest, HANDLER_OUTPUT,
      this->CTest->GetCTestConfiguration("DropSiteUser").c_str(), this->Quiet);
    if (!this->CTest->GetCTestConfiguration("DropSitePassword").empty()) {
      url += ":" + this->CTest->GetCTestConfiguration("DropSitePassword");
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, ":******", this->Quiet);
    }
    url += "@";
  }
  url += this->CTest->GetCTestConfiguration("DropSite") +
    this->CTest->GetCTestConfiguration("DropLocation");
}

int cmCTestSubmitHandler::HandleCDashUploadFile(std::string const& file,
                                                std::string const& typeString)
{
  if (file.empty()) {
    cmCTestLog(this->CTest, ERROR_MESSAGE, "Upload file not specified\n");
    return -1;
  }
  if (!cmSystemTools::FileExists(file)) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Upload file not found: '" << file << "'\n");
    return -1;
  }
  cmCTestCurl curl(this->CTest);
  curl.SetQuiet(this->Quiet);
  std::string curlopt(this->CTest->GetCTestConfiguration("CurlOptions"));
  std::vector<std::string> args;
  cmSystemTools::ExpandListArgument(curlopt, args);
  curl.SetCurlOptions(args);
  curl.SetTimeOutSeconds(SUBMIT_TIMEOUT_IN_SECONDS_DEFAULT);
  curl.SetHttpHeaders(this->HttpHeaders);
  std::string dropMethod;
  std::string url;
  this->ConstructCDashURL(dropMethod, url);
  std::string fields;
  std::string::size_type pos = url.find('?');
  if (pos != std::string::npos) {
    fields = url.substr(pos + 1);
    url = url.substr(0, pos);
  }
  if (!(dropMethod == "http" || dropMethod == "https")) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Only http and https are supported for CDASH_UPLOAD\n");
    return -1;
  }
  bool internalTest = cmSystemTools::IsOn(this->GetOption("InternalTest"));

  // Get RETRY_COUNT and RETRY_DELAY values if they were set.
  std::string retryDelayString = this->GetOption("RetryDelay") == nullptr
    ? ""
    : this->GetOption("RetryDelay");
  std::string retryCountString = this->GetOption("RetryCount") == nullptr
    ? ""
    : this->GetOption("RetryCount");
  auto retryDelay = std::chrono::seconds(0);
  if (!retryDelayString.empty()) {
    unsigned long retryDelayValue = 0;
    if (!cmSystemTools::StringToULong(retryDelayString.c_str(),
                                      &retryDelayValue)) {
      cmCTestLog(this->CTest, WARNING,
                 "Invalid value for 'RETRY_DELAY' : " << retryDelayString
                                                      << std::endl);
    } else {
      retryDelay = std::chrono::seconds(retryDelayValue);
    }
  }
  unsigned long retryCount = 0;
  if (!retryCountString.empty()) {
    if (!cmSystemTools::StringToULong(retryCountString.c_str(), &retryCount)) {
      cmCTestLog(this->CTest, WARNING,
                 "Invalid value for 'RETRY_DELAY' : " << retryCountString
                                                      << std::endl);
    }
  }

  std::string md5sum =
    cmSystemTools::ComputeFileHash(file, cmCryptoHash::AlgoMD5);
  // 1. request the buildid and check to see if the file
  //    has already been uploaded
  // TODO I added support for subproject. You would need to add
  // a "&subproject=subprojectname" to the first POST.
  cmCTestScriptHandler* ch =
    static_cast<cmCTestScriptHandler*>(this->CTest->GetHandler("script"));
  cmake* cm = ch->GetCMake();
  const char* subproject = cm->GetState()->GetGlobalProperty("SubProject");
  // TODO: Encode values for a URL instead of trusting caller.
  std::ostringstream str;
  if (subproject) {
    str << "subproject=" << curl.Escape(subproject) << "&";
  }
  auto timeNow =
    std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  str << "stamp=" << curl.Escape(this->CTest->GetCurrentTag()) << "-"
      << curl.Escape(this->CTest->GetTestModelString()) << "&"
      << "model=" << curl.Escape(this->CTest->GetTestModelString()) << "&"
      << "build="
      << curl.Escape(this->CTest->GetCTestConfiguration("BuildName")) << "&"
      << "site=" << curl.Escape(this->CTest->GetCTestConfiguration("Site"))
      << "&"
      << "track=" << curl.Escape(this->CTest->GetTestModelString()) << "&"
      << "starttime=" << timeNow << "&"
      << "endtime=" << timeNow << "&"
      << "datafilesmd5[0]=" << md5sum << "&"
      << "type=" << curl.Escape(typeString);
  if (!fields.empty()) {
    fields += '&';
  }
  fields += str.str();
  cmCTestOptionalLog(this->CTest, DEBUG,
                     "fields: " << fields << "\nurl:" << url
                                << "\nfile: " << file << "\n",
                     this->Quiet);
  std::string response;

  bool requestSucceeded = curl.HttpRequest(url, fields, response);
  if (!internalTest && !requestSucceeded) {
    // If request failed, wait and retry.
    for (unsigned long i = 0; i < retryCount; i++) {
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Request failed, waiting " << retryDelay.count()
                                                       << " seconds...\n",
                         this->Quiet);

      auto stop = std::chrono::steady_clock::now() + retryDelay;
      while (std::chrono::steady_clock::now() < stop) {
        cmSystemTools::Delay(100);
      }

      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Retry request: Attempt "
                           << (i + 1) << " of " << retryCount << std::endl,
                         this->Quiet);

      requestSucceeded = curl.HttpRequest(url, fields, response);
      if (requestSucceeded) {
        break;
      }
    }
  }
  if (!internalTest && !requestSucceeded) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Error in HttpRequest\n"
                 << response);
    return -1;
  }
  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                     "Request upload response: [" << response << "]\n",
                     this->Quiet);
  Json::Value json;
  Json::Reader reader;
  if (!internalTest && !reader.parse(response, json)) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "error parsing json string ["
                 << response << "]\n"
                 << reader.getFormattedErrorMessages() << "\n");
    return -1;
  }
  if (!internalTest && json["status"].asInt() != 0) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Bad status returned from CDash: " << json["status"].asInt());
    return -1;
  }
  if (!internalTest) {
    if (json["datafilesmd5"].isArray()) {
      int datares = json["datafilesmd5"][0].asInt();
      if (datares == 1) {
        cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                           "File already exists on CDash, skip upload "
                             << file << "\n",
                           this->Quiet);
        return 0;
      }
    } else {
      cmCTestLog(this->CTest, ERROR_MESSAGE,
                 "bad datafilesmd5 value in response " << response << "\n");
      return -1;
    }
  }

  std::string upload_as = cmSystemTools::GetFilenameName(file);
  std::ostringstream fstr;
  fstr << "type=" << curl.Escape(typeString) << "&"
       << "md5=" << md5sum << "&"
       << "filename=" << curl.Escape(upload_as) << "&"
       << "buildid=" << json["buildid"].asString();

  bool uploadSucceeded = false;
  if (!internalTest) {
    uploadSucceeded = curl.UploadFile(file, url, fstr.str(), response);
  }

  if (!uploadSucceeded) {
    // If upload failed, wait and retry.
    for (unsigned long i = 0; i < retryCount; i++) {
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Upload failed, waiting " << retryDelay.count()
                                                      << " seconds...\n",
                         this->Quiet);

      auto stop = std::chrono::steady_clock::now() + retryDelay;
      while (std::chrono::steady_clock::now() < stop) {
        cmSystemTools::Delay(100);
      }

      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Retry upload: Attempt "
                           << (i + 1) << " of " << retryCount << std::endl,
                         this->Quiet);

      if (!internalTest) {
        uploadSucceeded = curl.UploadFile(file, url, fstr.str(), response);
      }
      if (uploadSucceeded) {
        break;
      }
    }
  }

  if (!uploadSucceeded) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "error uploading to CDash. " << file << " " << url << " "
                                            << fstr.str());
    return -1;
  }
  if (!reader.parse(response, json)) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "error parsing json string ["
                 << response << "]\n"
                 << reader.getFormattedErrorMessages() << "\n");
    return -1;
  }
  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT,
                     "Upload file response: [" << response << "]\n",
                     this->Quiet);
  return 0;
}

int cmCTestSubmitHandler::ProcessHandler()
{
  const char* cdashUploadFile = this->GetOption("CDashUploadFile");
  const char* cdashUploadType = this->GetOption("CDashUploadType");
  if (cdashUploadFile && cdashUploadType) {
    return this->HandleCDashUploadFile(cdashUploadFile, cdashUploadType);
  }
  std::string iscdash = this->CTest->GetCTestConfiguration("IsCDash");
  // cdash does not need to trigger so just return true
  if (!iscdash.empty()) {
    this->CDash = true;
  }

  const std::string& buildDirectory =
    this->CTest->GetCTestConfiguration("BuildDirectory");
  if (buildDirectory.empty()) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Cannot find BuildDirectory  key in the DartConfiguration.tcl"
                 << std::endl);
    return -1;
  }

  if (getenv("HTTP_PROXY")) {
    this->HTTPProxyType = 1;
    this->HTTPProxy = getenv("HTTP_PROXY");
    if (getenv("HTTP_PROXY_PORT")) {
      this->HTTPProxy += ":";
      this->HTTPProxy += getenv("HTTP_PROXY_PORT");
    }
    if (getenv("HTTP_PROXY_TYPE")) {
      std::string type = getenv("HTTP_PROXY_TYPE");
      // HTTP/SOCKS4/SOCKS5
      if (type == "HTTP") {
        this->HTTPProxyType = 1;
      } else if (type == "SOCKS4") {
        this->HTTPProxyType = 2;
      } else if (type == "SOCKS5") {
        this->HTTPProxyType = 3;
      }
    }
    if (getenv("HTTP_PROXY_USER")) {
      this->HTTPProxyAuth = getenv("HTTP_PROXY_USER");
    }
    if (getenv("HTTP_PROXY_PASSWD")) {
      this->HTTPProxyAuth += ":";
      this->HTTPProxyAuth += getenv("HTTP_PROXY_PASSWD");
    }
  }

  if (getenv("FTP_PROXY")) {
    this->FTPProxyType = 1;
    this->FTPProxy = getenv("FTP_PROXY");
    if (getenv("FTP_PROXY_PORT")) {
      this->FTPProxy += ":";
      this->FTPProxy += getenv("FTP_PROXY_PORT");
    }
    if (getenv("FTP_PROXY_TYPE")) {
      std::string type = getenv("FTP_PROXY_TYPE");
      // HTTP/SOCKS4/SOCKS5
      if (type == "HTTP") {
        this->FTPProxyType = 1;
      } else if (type == "SOCKS4") {
        this->FTPProxyType = 2;
      } else if (type == "SOCKS5") {
        this->FTPProxyType = 3;
      }
    }
  }

  if (!this->HTTPProxy.empty()) {
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       "   Use HTTP Proxy: " << this->HTTPProxy << std::endl,
                       this->Quiet);
  }
  if (!this->FTPProxy.empty()) {
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       "   Use FTP Proxy: " << this->FTPProxy << std::endl,
                       this->Quiet);
  }
  cmGeneratedFileStream ofs;
  this->StartLogFile("Submit", ofs);

  std::vector<std::string> files;
  std::string prefix = this->GetSubmitResultsPrefix();

  if (!this->Files.empty()) {
    // Submit the explicitly selected files:
    //
    files.insert(files.end(), this->Files.begin(), this->Files.end());
  }

  // Add to the list of files to submit from any selected, existing parts:
  //

  // TODO:
  // Check if test is enabled

  this->CTest->AddIfExists(cmCTest::PartUpdate, "Update.xml");
  this->CTest->AddIfExists(cmCTest::PartConfigure, "Configure.xml");
  this->CTest->AddIfExists(cmCTest::PartBuild, "Build.xml");
  this->CTest->AddIfExists(cmCTest::PartTest, "Test.xml");
  if (this->CTest->AddIfExists(cmCTest::PartCoverage, "Coverage.xml")) {
    std::vector<std::string> gfiles;
    std::string gpath =
      buildDirectory + "/Testing/" + this->CTest->GetCurrentTag();
    std::string::size_type glen = gpath.size() + 1;
    gpath = gpath + "/CoverageLog*";
    cmCTestOptionalLog(this->CTest, DEBUG,
                       "Globbing for: " << gpath << std::endl, this->Quiet);
    if (cmSystemTools::SimpleGlob(gpath, gfiles, 1)) {
      for (std::string& gfile : gfiles) {
        gfile = gfile.substr(glen);
        cmCTestOptionalLog(this->CTest, DEBUG,
                           "Glob file: " << gfile << std::endl, this->Quiet);
        this->CTest->AddSubmitFile(cmCTest::PartCoverage, gfile.c_str());
      }
    } else {
      cmCTestLog(this->CTest, ERROR_MESSAGE, "Problem globbing" << std::endl);
    }
  }
  this->CTest->AddIfExists(cmCTest::PartMemCheck, "DynamicAnalysis.xml");
  this->CTest->AddIfExists(cmCTest::PartMemCheck, "Purify.xml");
  this->CTest->AddIfExists(cmCTest::PartNotes, "Notes.xml");
  this->CTest->AddIfExists(cmCTest::PartUpload, "Upload.xml");

  // Query parts for files to submit.
  for (cmCTest::Part p = cmCTest::PartStart; p != cmCTest::PartCount;
       p = cmCTest::Part(p + 1)) {
    // Skip parts we are not submitting.
    if (!this->SubmitPart[p]) {
      continue;
    }

    // Submit files from this part.
    std::vector<std::string> const& pfiles = this->CTest->GetSubmitFiles(p);
    files.insert(files.end(), pfiles.begin(), pfiles.end());
  }

  // Make sure files are unique, but preserve order.
  {
    // This endPos intermediate is needed to work around non-conformant C++11
    // standard libraries that have erase(iterator,iterator) instead of
    // erase(const_iterator,const_iterator).
    size_t endPos = cmRemoveDuplicates(files) - files.cbegin();
    files.erase(files.begin() + endPos, files.end());
  }

  // Submit Done.xml last
  if (this->SubmitPart[cmCTest::PartDone]) {
    files.push_back("Done.xml");
  }

  if (ofs) {
    ofs << "Upload files:" << std::endl;
    int cnt = 0;
    for (std::string const& file : files) {
      ofs << cnt << "\t" << file << std::endl;
      cnt++;
    }
  }
  cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                     "Submit files (using "
                       << this->CTest->GetCTestConfiguration("DropMethod")
                       << ")" << std::endl,
                     this->Quiet);
  const char* specificTrack = this->CTest->GetSpecificTrack();
  if (specificTrack) {
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       "   Send to track: " << specificTrack << std::endl,
                       this->Quiet);
  }
  this->SetLogFile(&ofs);

  std::string dropMethod(this->CTest->GetCTestConfiguration("DropMethod"));

  if (dropMethod.empty() || dropMethod == "ftp") {
    ofs << "Using drop method: FTP" << std::endl;
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       "   Using FTP submit method" << std::endl
                                                    << "   Drop site: ftp://",
                       this->Quiet);
    std::string url = "ftp://";
    url += cmCTest::MakeURLSafe(
             this->CTest->GetCTestConfiguration("DropSiteUser")) +
      ":" +
      cmCTest::MakeURLSafe(
             this->CTest->GetCTestConfiguration("DropSitePassword")) +
      "@" + this->CTest->GetCTestConfiguration("DropSite") +
      cmCTest::MakeURLSafe(this->CTest->GetCTestConfiguration("DropLocation"));
    if (!this->CTest->GetCTestConfiguration("DropSiteUser").empty()) {
      cmCTestOptionalLog(
        this->CTest, HANDLER_OUTPUT,
        this->CTest->GetCTestConfiguration("DropSiteUser").c_str(),
        this->Quiet);
      if (!this->CTest->GetCTestConfiguration("DropSitePassword").empty()) {
        cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, ":******",
                           this->Quiet);
      }
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, "@", this->Quiet);
    }
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       this->CTest->GetCTestConfiguration("DropSite")
                         << this->CTest->GetCTestConfiguration("DropLocation")
                         << std::endl,
                       this->Quiet);
    if (!this->SubmitUsingFTP(buildDirectory + "/Testing/" +
                                this->CTest->GetCurrentTag(),
                              files, prefix, url)) {
      cmCTestLog(this->CTest, ERROR_MESSAGE,
                 "   Problems when submitting via FTP" << std::endl);
      ofs << "   Problems when submitting via FTP" << std::endl;
      return -1;
    }
    if (!this->CDash) {
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Using HTTP trigger method"
                           << std::endl
                           << "   Trigger site: "
                           << this->CTest->GetCTestConfiguration("TriggerSite")
                           << std::endl,
                         this->Quiet);
      if (!this->TriggerUsingHTTP(
            files, prefix,
            this->CTest->GetCTestConfiguration("TriggerSite"))) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Problems when triggering via HTTP" << std::endl);
        ofs << "   Problems when triggering via HTTP" << std::endl;
        return -1;
      }
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Submission successful" << std::endl, this->Quiet);
      ofs << "   Submission successful" << std::endl;
      return 0;
    }
  } else if (dropMethod == "http" || dropMethod == "https") {
    std::string url = dropMethod;
    url += "://";
    ofs << "Using drop method: " << dropMethod << std::endl;
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       "   Using HTTP submit method" << std::endl
                                                     << "   Drop site:" << url,
                       this->Quiet);
    if (!this->CTest->GetCTestConfiguration("DropSiteUser").empty()) {
      url += this->CTest->GetCTestConfiguration("DropSiteUser");
      cmCTestOptionalLog(
        this->CTest, HANDLER_OUTPUT,
        this->CTest->GetCTestConfiguration("DropSiteUser").c_str(),
        this->Quiet);
      if (!this->CTest->GetCTestConfiguration("DropSitePassword").empty()) {
        url += ":" + this->CTest->GetCTestConfiguration("DropSitePassword");
        cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, ":******",
                           this->Quiet);
      }
      url += "@";
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT, "@", this->Quiet);
    }
    url += this->CTest->GetCTestConfiguration("DropSite") +
      this->CTest->GetCTestConfiguration("DropLocation");
    cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                       this->CTest->GetCTestConfiguration("DropSite")
                         << this->CTest->GetCTestConfiguration("DropLocation")
                         << std::endl,
                       this->Quiet);
    if (!this->SubmitUsingHTTP(buildDirectory + "/Testing/" +
                                 this->CTest->GetCurrentTag(),
                               files, prefix, url)) {
      cmCTestLog(this->CTest, ERROR_MESSAGE,
                 "   Problems when submitting via HTTP" << std::endl);
      ofs << "   Problems when submitting via HTTP" << std::endl;
      return -1;
    }
    if (!this->CDash) {
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Using HTTP trigger method"
                           << std::endl
                           << "   Trigger site: "
                           << this->CTest->GetCTestConfiguration("TriggerSite")
                           << std::endl,
                         this->Quiet);
      if (!this->TriggerUsingHTTP(
            files, prefix,
            this->CTest->GetCTestConfiguration("TriggerSite"))) {
        cmCTestLog(this->CTest, ERROR_MESSAGE,
                   "   Problems when triggering via HTTP" << std::endl);
        ofs << "   Problems when triggering via HTTP" << std::endl;
        return -1;
      }
    }
    if (this->HasErrors) {
      cmCTestLog(this->CTest, HANDLER_OUTPUT,
                 "   Errors occurred during "
                 "submission."
                   << std::endl);
      ofs << "   Errors occurred during submission. " << std::endl;
    } else {
      cmCTestOptionalLog(this->CTest, HANDLER_OUTPUT,
                         "   Submission successful"
                           << (this->HasWarnings ? ", with warnings." : "")
                           << std::endl,
                         this->Quiet);
      ofs << "   Submission successful"
          << (this->HasWarnings ? ", with warnings." : "") << std::endl;
    }

    return 0;
  }

  cmCTestLog(this->CTest, ERROR_MESSAGE,
             "   Unknown submission method: \"" << dropMethod << "\""
                                                << std::endl);
  return -1;
}

std::string cmCTestSubmitHandler::GetSubmitResultsPrefix()
{
  std::string buildname =
    cmCTest::SafeBuildIdField(this->CTest->GetCTestConfiguration("BuildName"));
  std::string name = this->CTest->GetCTestConfiguration("Site") + "___" +
    buildname + "___" + this->CTest->GetCurrentTag() + "-" +
    this->CTest->GetTestModelString() + "___XML___";
  return name;
}

void cmCTestSubmitHandler::SelectParts(std::set<cmCTest::Part> const& parts)
{
  // Check whether each part is selected.
  for (cmCTest::Part p = cmCTest::PartStart; p != cmCTest::PartCount;
       p = cmCTest::Part(p + 1)) {
    this->SubmitPart[p] =
      (std::set<cmCTest::Part>::const_iterator(parts.find(p)) != parts.end());
  }
}

void cmCTestSubmitHandler::SelectFiles(cmCTest::SetOfStrings const& files)
{
  this->Files.insert(files.begin(), files.end());
}
