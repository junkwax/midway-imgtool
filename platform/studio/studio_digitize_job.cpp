#include "studio_digitize_job.h"
#include <stdexcept>
namespace studio {
namespace {
std::unique_ptr<DigitizeSession> matte_input(const DigitizeSession &s){
    auto out=std::make_unique<DigitizeSession>();out->materials=s.materials;out->matte=s.matte;out->seed=s.seed;out->key=s.key;out->trim=s.trim;out->custom_anchor=s.custom_anchor;out->sequence=s.sequence;out->allow_unassigned=s.allow_unassigned;out->scale=s.scale;out->anchor_x=s.anchor_x;out->anchor_y=s.anchor_y;out->palette=s.palette;out->prefix=s.prefix;out->revision=s.revision;
    out->frames.reserve(s.frames.size());for(auto &f:s.frames){DigitizeWork frame;frame.source=f.source;frame.included=f.included;out->frames.push_back(std::move(frame));}return out;
}
}
const char *digitize_stage_label(DigitizeStage stage){switch(stage){case DigitizeStage::Loading:return "Loading sources";case DigitizeStage::Matte:return "Building mattes";case DigitizeStage::Materials:return "Fitting materials";case DigitizeStage::Classify:return "Classifying frames";case DigitizeStage::Map:return "Mapping palette colors";}return "Processing";}
DigitizeJob::~DigitizeJob(){cancel=true;if(thread.joinable())thread.join();}
bool DigitizeJob::matches(uint64_t uid,uint64_t art_generation,uint64_t review_epoch)const{return finished.load(std::memory_order_acquire)&&ok&&!cancel.load()&&owner==uid&&generation==art_generation&&epoch==review_epoch;}
void DigitizeJob::start(){if(thread.joinable())throw std::logic_error("Digitize job already started.");thread=std::thread([this]{try{
    DigitizeControl control;control.cancelled=[this]{return cancel.load();};control.progress=[this](DigitizeStage s,int n,int count){stage=s;total=count;done=n;};
    if(!input){error="The digitize snapshot is unavailable.";}
    else if(!cancel){if(kind==DigitizeJobKind::Load){std::vector<DigitizeSource> sources;if(load_digitize_sources(paths,sources,error,control)){auto result=std::make_unique<DigitizeSession>();if(initialize_digitize(std::move(sources),*result,error)&&rebuild_digitize_matte(*result,error,control)){output=std::move(result);ok=true;}}}
        else{auto result=kind==DigitizeJobKind::Matte?matte_input(*input):std::make_unique<DigitizeSession>(*input);if(!cancel&&(kind==DigitizeJobKind::Matte?rebuild_digitize_matte(*result,error,control):kind==DigitizeJobKind::Reseed?classify_digitize_checked(*result,true,control):fit_digitize_palette(target,*result,error,control))){output=std::move(result);ok=true;}}
    }
    if(cancel){output.reset();ok=false;}
}catch(const std::exception &e){error=std::string("Digitize processing failed: ")+e.what();ok=false;output.reset();}catch(...){error="Digitize processing failed.";ok=false;output.reset();}finished.store(true,std::memory_order_release);});}
}
