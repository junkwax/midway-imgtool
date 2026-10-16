#include "studio_fatality_workspace.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <new>
static long fail_allocation=-1;
void *operator new(std::size_t n){if(fail_allocation==0){fail_allocation=-1;throw std::bad_alloc();}if(fail_allocation>0)--fail_allocation;if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void *p)noexcept{std::free(p);}void operator delete(void *p,std::size_t)noexcept{std::free(p);}
static int failures=0;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": " #x " / "<<error<<"\n";++failures;}}while(0)
static std::string read(const std::filesystem::path &path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void write(const std::filesystem::path &path,const std::string &bytes){std::ofstream f(path,std::ios::binary);f<<bytes;}
int main(){
    using namespace studio;namespace fs=std::filesystem;std::string error;
    auto root=fs::temp_directory_path()/("studio-fatality-workspace-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
    auto ap=root/"MOVES.ASM",bp=root/fs::u8path(u8"buttons \u65e5.ASM"),cp=root/"other.ASM";
    std::string a="fatal_demo\n\tmovi r_fatal,a7\n\tmovi scom_shared,a11\n\tjmp fatality_xfer\n";
    std::string b="scom_shared\n\t.word 20,sw_up\n",c="scom_shared\n\t.word 30,sw_down\n";
    write(ap,a);write(bp,b);write(cp,c);FatalityWorkspace workspace;
    auto aid=workspace.add(ap.u8string(),error),bid=workspace.add(bp.u8string(),error);CHECK(aid&&bid&&aid!=bid&&workspace.documents().size()==2);
    auto ad=workspace.document(aid),bd=workspace.document(bid);CHECK(ad&&bd&&!workspace.document(999)&&workspace.dirty_count()==0);
    CHECK(workspace.add((root/"."/"MOVES.ASM").u8string(),error)==aid&&workspace.document(aid)==ad&&workspace.documents().size()==2);
    std::error_code ec;auto alias=root/"alias.ASM";fs::create_hard_link(ap,alias,ec);if(!ec)CHECK(workspace.add(alias.u8string(),error)==aid&&workspace.documents().size()==2);
    FatalityPatch patch;CHECK(ad->review(0,ad->body(0)+"\t; command change\n",patch,error)&&ad->apply(patch,error));
    CHECK(bd->review(0,"\t.word 35,sw_up\n",patch,error)&&bd->apply(patch,error));
    auto agen=ad->generation(),bgen=bd->generation();CHECK(workspace.dirty_count()==2&&workspace.first_dirty()==aid&&read(ap)==a&&read(bp)==b);
    CHECK(workspace.add(ap.u8string(),error)==aid&&workspace.document(aid)->dirty()&&workspace.document(aid)==ad);
    CHECK(ad->undo(error)&&!ad->dirty()&&bd->dirty()&&bd->generation()==bgen&&workspace.first_dirty()==bid);
    CHECK(ad->redo(error)&&ad->dirty()&&bd->dirty()&&bd->generation()==bgen);
    auto matches=workspace.matches("SCOM_SHARED",FatalityKind::Combo,aid);CHECK(matches.size()==1&&matches[0].document==bid);
    std::shared_ptr<FatalitySource> linked;CHECK(workspace.resolve(matches[0],linked,error)&&linked==bd);
    auto stale=matches[0];CHECK(bd->undo(error)&&!workspace.resolve(stale,linked,error));CHECK(bd->redo(error));
    auto cid=workspace.add(cp.u8string(),error);CHECK(cid&&workspace.matches("scom_shared",FatalityKind::Combo,aid).size()==2);
    CHECK(workspace.matches("scom_shared",FatalityKind::Combo,bid).size()==1&&workspace.matches("scom_shared",FatalityKind::Combo,bid)[0].document==bid);
    CHECK(workspace.matches("scom_shared",FatalityKind::Animation,aid).empty());
    CHECK(!workspace.close(aid,false,error)&&workspace.document(aid)==ad&&ad->dirty());
    CHECK(!workspace.adopt(FatalitySource::open(cp.u8string(),error),aid,true,error)&&workspace.document(aid)==ad);
    auto replacement=root/"replacement.ASM";write(replacement,"scom_local\n\t.word 60\n");
    CHECK(!workspace.adopt(FatalitySource::open(replacement.u8string(),error),aid,false,error)&&workspace.document(aid)==ad);
    // All allocations for replacement happen before swapping an owned source.
    int injected=0;uint64_t adopted=0;auto before=ad->bytes();agen=ad->generation();
    for(long i=0;i<1000;++i){auto candidate=FatalitySource::open(replacement.u8string(),error);fail_allocation=i;adopted=workspace.adopt(std::move(candidate),aid,true,error);fail_allocation=-1;if(adopted)break;++injected;CHECK(workspace.document(aid)==ad&&ad->bytes()==before&&ad->generation()==agen&&workspace.documents().size()==3&&bd->dirty());}
    CHECK(adopted==aid&&injected>2&&workspace.document(aid)!=ad&&workspace.document(aid)->path()==replacement.u8string()&&workspace.document(bid)==bd&&ad->dirty());
    // A resident source is never overwritten through another source's Save As.
    FatalityWriteReview save;CHECK(!workspace.review_save(aid,bp.u8string(),save,error));
    CHECK(workspace.review_save(bid,bp.u8string(),save,error));CHECK(!workspace.save(aid,save,error)&&read(bp)==b);
    CHECK(workspace.save(bid,save,error)&&!bd->dirty()&&workspace.dirty_count()==0&&read(bp)==bd->bytes());
    auto new_path=root/"saved-copy.ASM";CHECK(workspace.review_save(bid,new_path.u8string(),save,error)&&workspace.save(bid,save,error));
    CHECK(workspace.find_path(new_path.u8string())==bid&&workspace.find_path(bp.u8string())==0&&workspace.add(new_path.u8string(),error)==bid);
    auto b2=workspace.add(bp.u8string(),error);CHECK(b2&&b2!=bid&&workspace.documents().size()==4);
    CHECK(!workspace.review_save(b2,new_path.u8string(),save,error));
    // A newly opened source can claim a formerly free output after review.
    auto claimed=root/"claimed.ASM";CHECK(workspace.review_save(bid,claimed.u8string(),save,error));write(claimed,c);
    auto claimed_id=workspace.add(claimed.u8string(),error);CHECK(claimed_id&&!workspace.save(bid,save,error)&&read(claimed)==c);
    auto before_reload=workspace.matches("scom_shared",FatalityKind::Combo,cid)[0];
    CHECK(workspace.adopt(FatalitySource::open(cp.u8string(),error),cid,false,error)==cid&&!workspace.resolve(before_reload,linked,error));
    auto reference=workspace.matches("scom_shared",FatalityKind::Combo,cid)[0];CHECK(workspace.close(cid,false,error)&&!workspace.resolve(reference,linked,error));
    auto reopened=workspace.add(cp.u8string(),error);CHECK(reopened>cid&&!workspace.document(cid));
    CHECK(!workspace.add((root/"missing.ASM").u8string(),error)&&workspace.documents().size()==5);
    // Add is transactional too: failed preparation preserves all resident edits.
    auto added_path=root/"added.ASM";write(added_path,"scom_added\n\t.word 10\n");uint64_t added=0;injected=0;auto added_name=added_path.u8string();
    for(long i=0;i<1500;++i){fail_allocation=i;added=workspace.add(added_name,error);fail_allocation=-1;if(added)break;++injected;CHECK(workspace.documents().size()==5&&workspace.document(bid)==bd&&read(new_path)==bd->bytes());}
    CHECK(added&&injected>10&&workspace.documents().size()==6);
    // Capacity failure never evicts an existing source or recycles its ID.
    for(size_t i=workspace.documents().size();i<FatalityWorkspace::limit;++i){auto p=root/("source-"+std::to_string(i)+".ASM");write(p,"scom_extra\n\t.word 5\n");CHECK(workspace.add(p.u8string(),error));}
    auto full=workspace.documents().size();CHECK(full==FatalityWorkspace::limit);
    auto overflow=root/"overflow.ASM";write(overflow,c);CHECK(!workspace.add(overflow.u8string(),error)&&workspace.documents().size()==full&&workspace.add(new_path.u8string(),error)==bid);
    auto current=workspace.document(aid);CHECK(current->review(0,"\t.word 99\n",patch,error)&&current->apply(patch,error));
    CHECK(!workspace.close(aid,false,error)&&workspace.close(aid,true,error)&&!workspace.document(aid)&&read(replacement)=="scom_local\n\t.word 60\n");
    CHECK(workspace.add(overflow.u8string(),error)>reopened&&workspace.documents().size()==full);
    CHECK(read(ap)==a&&read(cp)==c);fs::remove_all(root);std::cout<<"Fatality workspace: "<<failures<<" failures\n";return failures?1:0;
}
