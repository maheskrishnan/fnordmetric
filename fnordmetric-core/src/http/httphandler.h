/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#ifndef _FNORDMETRIC_HTTPHANDLER_H
#define _FNORDMETRIC_HTTPHANDLER_H
#include <fnordmetric/http/httprequest.h>
#include <fnordmetric/http/httpresponse.h>

namespace fnord {
namespace http {

class HTTPHandler {
public:

  virtual ~HTTPHandler() {}

  virtual bool handleHTTPRequest(
      HTTPRequest* request,
      HTTPResponse* response) = 0;

};

}
}
#endif
