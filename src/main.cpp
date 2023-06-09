#include <algorithm>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <poll.h>
#include <nlohmann/json.hpp>

#include <curl/curl.h>
#include "curl/easy.h"
#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
// #include "ftxui/component/component_options.hpp"
#include "ftxui/component/screen_interactive.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::map;

using json = nlohmann::json;

// static const uint32_t ID_SUBSCRIPTION = 1;
// static const uint32_t ID_GETSTATES = 2;
// static const uint32_t ID_START = 100;

class HAEntity
{
public:
  HAEntity(void) {

  }
  HAEntity(json _state) {
    state = _state;
  }
  ~HAEntity() {

  }

  void update(json _state) {
    state = _state;
  }

  std::string toString(void) {
    return state.dump(2);
  }

  std::vector<std::string> attrVector(void) {
    std::vector<std::string> ret;


    for (const auto &[k, v] : state["attributes"].items()) {
      ret.push_back(k+std::string(": ")+v.dump());
    }

    return ret;
  }

  std::string getState(void) {
    return state["state"];
  }

  std::string getInfo(void) {
    std::ostringstream ret;

    ret<<"state="<<getState()<<"  ";
    ret<<"domain="<<getDomain()<<"  ";
    // ret<<""
    return ret.str();
  }

  std::string getDomain(void) {
    auto id = state["entity_id"].get<std::string>();

    // FIXME: boost::split might be nice here, check if its header only?
    auto pos = id.find(".");

    if (pos == std::string::npos) {
      throw std::runtime_error("entity ID ["+id+"] contains no period, has no domain?");
    }

    return id.substr(0, pos);
  }
private:
  json state;
};

class HADomain
{
public:
  HADomain(void) {

  }
  HADomain(json _state) {
    state = _state;
  }
  ~HADomain() {

  }

  void update(json _state) {
    state = _state;
  }

  std::string toString(void) {
    return state.dump(2);
  }

  std::string getState(void) {
    return state["state"];
  }

  std::vector<std::string> getServices(void) {
    std::vector<std::string> ret;

    // cerr<<state.dump()<<endl;
    for (auto &[service,info] : state.items()) {
      ret.push_back(service);
    }

    return ret;
  }

private:
  json state;
};


// FIXME: combine states and stateslock so unlocked use becomes impossible
map<string, std::shared_ptr<HAEntity>> states;
std::mutex stateslock;

map<string, std::shared_ptr<HADomain>> domains;
std::mutex domainslock;

class WSConn
{
public:
  WSConn(std::string url) {
    wshandle = curl_easy_init();

    curl_easy_setopt(wshandle, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(wshandle, CURLOPT_URL, url.c_str());
    curl_easy_perform(wshandle);
  }
  ~WSConn() {
        // FIXME clean up here
  }

  std::string recv(void) {
    size_t recv;
    struct curl_ws_frame *meta;
    struct pollfd pfd;

    char buffer[8192];

    std::string result;

    pfd.events = POLLIN;
        /* cerr<< */ curl_easy_getinfo(wshandle, CURLINFO_ACTIVESOCKET, &pfd.fd) /* <<endl */ ;

    CURLcode ret;

        // FIXME: handle frames > 64k
    while(true) {
      {
        std::scoped_lock lk(wshandlelock);
        ret = curl_ws_recv(wshandle, buffer, sizeof(buffer), &recv, &meta);
      }
          // cerr<<"CURLE_AGAIN"<<endl;
      // cerr<<"recv ret="<<ret<<endl;
      if (ret == CURLE_OK) {
        std::string chunk(buffer, recv); // FIXME: string_view?
        result = result + chunk;
        // cerr<<"bytesleft="<<(meta->bytesleft)<<endl;
        // cerr<<"result.size()="<<result.size()<<endl;
        if (meta->bytesleft == 0) {
          break;
        }
      }
      else if (ret == CURLE_AGAIN) {
        poll(&pfd, 1, 1000);
      }
      else {
        throw std::runtime_error("got error from curl_ws_recv: "+std::string(curl_easy_strerror(ret))); // FIXME: does not hold wshandlelock, might even print the wrong error in theory
      }
    }
        // cerr<<"ret="<<ret<<endl;
        // cerr<<"buffer="<<buffer<<endl;
        // cerr<<"recv="<<recv<<endl;
        // cout<<"RESULT:"<<endl;
        // cout<<result<<endl;
        // cout<<"END RESULT"<<endl;
    return result;
  }

  void send(json& msg) {
    {
      std::scoped_lock lk(msgidlock);

      if (msgid) {
        // FIXME: at zero, we are authing, which does not get an id. this is a hack.
        msg["id"]=msgid;
      }

      msgid++;
    }

    auto jmsg = msg.dump();

    send(jmsg);
  }

  std::mutex wshandlelock;
  CURL* wshandle;

  std::mutex msgidlock;
  int msgid = 0;

private:
  // call with wshandlelock held
  void send(std::string& msg) {
    std::scoped_lock lk(wshandlelock);
    size_t sent;
    // cerr<<"sending: "<<msg<<endl;
    curl_ws_send(wshandle, msg.c_str(), msg.length(), &sent, 0, CURLWS_TEXT);
  }
};

std::vector<std::string> entries;
std::mutex entrieslock;

ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::FitComponent();

// FIXME: leaking ref from inside a lock
// FIXME: don't copy entire vectors
// FIXME: arg wants to be ref
std::vector<std::string> getServicesForDomain(std::string domain) {
  if (domains.count(domain)) {
    return domains[domain]->getServices();
  }
  else {
    return {};
  }
}

void uithread(WSConn& wc) {
  using namespace ftxui;

  int selected;
  int selected2;
  int selectedbutton;

  std::vector<std::string> entries2;
  entries2.push_back("hoi");
  entries2.push_back("hoi2");

  auto radiobox = Menu(&entries, &selected);
  auto radiobox2 = Menu(&entries2, &selected2);

  auto uirenderer = Container::Horizontal({});

  string pressed;

  auto renderer = Renderer(uirenderer, [&] {
    std::scoped_lock lk(entrieslock, stateslock, domainslock);

    // for(auto &[k,v] : domains) {
    //   cerr<<"domain "<<k<<"exists"<<endl;
    // }
    std::vector<std::string> services;
    // cerr<<"about to get services, selected=="<<selected<<" , entries.size=="<<entries.size()<<endl;
    if (selected >= 0 && entries.size() > 0) {
      // cerr<<"getting services"<<endl;
      services = getServicesForDomain(states.at(entries.at(selected))->getDomain());
    }

    std::vector<Component> buttons;
    for (const auto &service : services) {
      auto entity = entries.at(selected);

      // cerr<<service<<endl;
      buttons.push_back(Button(service, [&selected, &wc, service] {
        // cout<<"PUSHED: "<< entries.at(selected) << service<<endl;

        json cmd;

        cmd["type"]="call_service";
        cmd["domain"]=states.at(entries.at(selected))->getDomain();
        cmd["service"]=service;
        cmd["target"]["entity_id"]=entries.at(selected);

        wc.send(cmd);

      })); // FIXME: this use of entries.at is gross, should centralise the empty-entries-list fallback
    }

    // cerr<<"services.size()=="<<services.size()<<", buttons.size()=="<<buttons.size()<<endl;

    uirenderer->DetachAllChildren();
    uirenderer->Add(radiobox | vscroll_indicator | frame | /* size(HEIGHT, LESS_THAN, 15) | */ size(WIDTH, EQUAL, 60) | border);
    // if (selected >= 0 && entries.size() > 0)
    if (!buttons.empty()) { 
      auto buttonrenderer = Container::Vertical(buttons, &selectedbutton);
      uirenderer->Add(buttonrenderer | size(WIDTH, GREATER_THAN, 15));
    }

    std::vector<Element> attrs;
    if (selected >= 0 && entries.size() > 0) {
      for(const auto &attr : states.at(entries.at(selected))->attrVector()) {
        attrs.push_back(text(attr));
      }
    }

    return vbox(
              hbox(text("selected = "), text(selected >=0 && entries.size() ? entries.at(selected) : "")),
              text(selected >= 0 && entries.size() > 0 ? states.at(entries.at(selected))->getInfo() : "no info"),
              text(pressed),
                        // text("hi"),
            // hbox(text("selected2 = "), text(selected2 >=0 && entries2.size() ? entries2.at(selected2) : "")),
            // vbox(
              // {
                    // hbox(
                    //   {
                    //     // text("test") | border,
                    //     // paragraph(selected >= 0 && domains.size()>0 ? domains.at(states.at(entries.at(selected))->getDomain())->getServices()[0] : "")
                    //     // paragraph(selected >= 0 && entries.size() > 0 ? getServicesForDomain(states.at(entries.at(selected))->getDomain() )[0] : "")
                    //   }
                    // ),
              hbox({
                uirenderer->Render(),
                vbox(attrs)
              })
                  // }
                // )
            );
  });

  renderer |= CatchEvent([&](Event event) {
    if (event.is_character()) {
      auto c = event.character();

      if (c == "q") {
        screen.ExitLoopClosure()(); // FIXME: surely this can be cleaner
      }

      pressed += event.character();
    }
    return false;
  });
  // auto screen = ScreenInteractive::FitComponent();
  screen.Loop(renderer);
}

std::string GetEnv(std::string key)
{
  auto value = getenv(key.c_str());

  if (value == nullptr) {
    throw std::runtime_error("environment variable "+key+" not set, exiting");
  }

  return value;
}

void hathread(WSConn& wc) {

  auto welcome = wc.recv();

  auto jwelcome = json::parse(welcome);

  // cerr<<"got welcome: "<<welcome<<endl; // FIXME check that it is the expected auth_required message

  json auth;

  auth["type"] = "auth";
  auth["access_token"] = GetEnv("HA_API_TOKEN");

  // cerr<<auth.dump()<<endl;

  // cerr<<jauth<<endl;
  wc.send(auth);

  // cerr<<wc.recv()<<endl; // FIXME assert auth_ok

  json subscribe;

  subscribe["type"] = "subscribe_events";

  wc.send(subscribe);


  // json call;

  // call["type"]="call_service";
  // call["domain"]="light";
  // call["service"]="toggle";
  // call["target"]["entity_id"]="light.plafondlamp_kantoor_peter_level_light_color_on_off";

  // auto jcall = call.dump();

  // wc.send(jcall);

  json getstates;

  getstates["type"]="get_states";

  wc.send(getstates);

  json getdomains;

  getdomains["type"]="get_services";

  wc.send(getdomains);

/* example ID_SUBSCRIPTION message:
{
  "id": 1,
  "type": "event",
  "event": {
    "event_type": "state_changed",
    "data": {
      "entity_id": "sensor.shellyplug_s_bc6aa8_power",
      "old_state": {
...
      },
      "new_state": {
        "entity_id": "sensor.shellyplug_s_bc6aa8_power",
        "state": "9.89",
...
 */

/* example ID_GETSTATES message:
{
  "id": 2,
  "type": "result",
  "success": true,
  "result": [
    {
      "entity_id": "light.plafondlamp_kantoor_peter_level_light_color_on_off",
      "state": "on",
      "attributes": {
        "min_color_temp_kelvin": 2000,
        "max_color_temp_kelvin": 6535,

*/

  while (true) {
    auto msg = wc.recv();

    // cout<<msg<<endl;
    json j = json::parse(msg);


    {
      std::scoped_lock lk(stateslock);

      if (j["id"] == getstates["id"]) {
        for (auto evd : j["result"]) {
          // cerr<<evd.dump()<<endl;
          auto entity_id = evd["entity_id"];

          auto old_state = evd["old_state"];
          auto new_state = evd["new_state"];

          // cout << "entity_id=" << entity_id << ", ";
          // cout << "state=" << evd["state"];
          // cout << endl;

          states[entity_id] = std::make_shared<HAEntity>(evd);
        }
        // exit(1);
      }
      else if (j["id"] == getdomains["id"]) {
        cerr<<j.dump()<<endl;
        for (auto &[domain,_services] : j["result"].items()) {
          // cerr<<service.dump()<<endl;


          // cout << "entity_id=" << entity_id << ", ";
          // cout << "state=" << evd["state"];
          // cout << endl;

          domains[domain] = std::make_shared<HADomain>(_services);
          // cerr<<"got services for domain "<<domain<<endl;
        }
        // exit(1);
      }
      else if (j["type"] == "event") {
        auto event = j["event"];
        auto event_type = event["event_type"];
        auto evd = event["data"];
        auto entity_id = evd["entity_id"];

        auto old_state = evd["old_state"];
        auto new_state = evd["new_state"];

        // cout << "event_type=" << event_type << ", ";
        // cout << "entity_id=" << entity_id << ", ";
        // cout << "state=" << new_state["state"];
        // cout << endl;

        if (event_type == "state_changed") {
          states[entity_id] = std::make_shared<HAEntity>(new_state);
        }
      }
      else {
        // not a message we were expecting
        continue;
      }

    }
    // cerr<<"\033[2Jhave "<<states.size()<< " states" << endl;
    // cerr<<"selected = "<<selected<<endl;
    // cerr<<endl;
    // for (auto &[k,v] : states) {
    //   cout<<k<<"="<<v->getState()<<endl;
    // }
    { 
      std::scoped_lock lk(entrieslock);
      entries.clear();

      for (auto &[k,v] : states) {
        entries.push_back(k); // +":"+v->getState());
      }
    }

    screen.PostEvent(ftxui::Event::Custom);
  }
}

int main(void) // int /* argc */, char* /* argv[] */*)
{
  curl_global_init(CURL_GLOBAL_ALL);
  auto wc = WSConn(GetEnv("HA_WS_URL"));

  std::thread ui(uithread, std::ref(wc));
  std::thread ha(hathread, std::ref(wc));

  ha.detach();
  ui.join();
}
