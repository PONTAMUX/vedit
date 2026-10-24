#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include <cstdio>
#include <string>
using namespace Steinberg; using namespace Steinberg::Vst;
static std::string u8(const TChar* s){ std::string r; for(;*s;++s) r += (*s<128)?char(*s):'?'; return r; }
int main(int argc,char**argv){
  std::string err;
  auto mod = VST3::Hosting::Module::create(argv[1], err);
  if(!mod){ printf("load fail %s\n", err.c_str()); return 1; }
  auto& f = mod->getFactory();
  for(auto& ci : f.classInfos()) printf("class %s cat=%s\n", ci.name().c_str(), ci.category().c_str());
  static HostApplication host;
  PluginContextFactory::instance().setPluginContext(&host);
  for(auto& ci : f.classInfos()){
    if(ci.category()!=kVstAudioEffectClass) continue;
    // simple
    auto comp = f.createInstance<IComponent>(ci.ID());
    if(!comp){ printf("no comp\n"); return 2; }
    printf("initialize %d\n", comp->initialize(&host));
    IPtr<IEditController> ctl;
    FUnknownPtr<IEditController> sc(comp);
    if(sc) ctl = sc; else {
      TUID cid; if(comp->getControllerClassId(cid)==kResultTrue){
        auto c = f.createInstance<IEditController>(VST3::UID::fromTUID(cid)); ctl = c; if(ctl) ctl->initialize(&host);
      }
    }
    FUnknownPtr<IAudioProcessor> ap(comp);
    printf("latency %u\n", ap->getLatencySamples());
    int n = ctl->getParameterCount(); printf("params %d\n", n);
    for(int i=0;i<n;i++){ ParameterInfo pi; ctl->getParameterInfo(i,pi);
      printf("%d id=%u '%s' units='%s' steps=%d def=%g flags=%d\n", i, pi.id, u8(pi.title).c_str(), u8(pi.units).c_str(), pi.stepCount, pi.defaultNormalizedValue, pi.flags); }
  }
  return 0;
}
