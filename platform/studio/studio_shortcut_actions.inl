// Pixel clipboard -> one transactional frame import; no IMG layout conversion.
void paste_new_sprite(Tab &t){
    if(clipboard.w<1||clipboard.h<1||clipboard.pixels.size()!=(size_t)clipboard.w*clipboard.h||clipboard.colors.empty())return;
    ImportBatch batch;batch.sequence=false;batch.words=clipboard.colors;batch.palette=-1;
    for(int i=0;i<t.art->palette_count();++i)if(copy_palette(*t.art,i).words==batch.words){batch.palette=i;break;}
    ImportFrame frame;frame.w=clipboard.w;frame.h=clipboard.h;int stride=(frame.w+3)&~3;frame.pixels.resize((size_t)stride*frame.h);
    for(int y=0;y<frame.h;++y)for(int x=0;x<frame.w;++x){size_t src=(size_t)y*frame.w+x;if(clipboard.mask.empty()||clipboard.mask[src])frame.pixels[(size_t)y*stride+x]=clipboard.pixels[src];}
    batch.frames.push_back(std::move(frame));if(!name_import_batch(*t.art,"PASTED",batch,error))return;
    int added=t.art->import_batch(batch,error);if(added>=0){select(added);status="Pasted pixels as a new sprite. Undo restores the entire import.";}
}
void reorder_preview(Tab &t,int delta){
    if(t.embedded.enabled){t.embedded.command=delta<0?EmbeddedCommand::Left:EmbeddedCommand::Right;return;}
    if(t.project&&t.alignment.enabled){t.scene.command=delta<0?SceneCommand::MoveLeft:SceneCommand::MoveRight;return;}
    auto &v=t.alignment;int from=v.player.frame,to=from+delta;
    if(v.sequence<0||v.sequence>=(int)v.sequences.size()||to<0||to>=(int)v.sequences[v.sequence].frames.size())return;
    std::swap(v.sequences[v.sequence].frames[from],v.sequences[v.sequence].frames[to]);std::swap(v.holds[v.sequence][from],v.holds[v.sequence][to]);select_frame(t,to);
    status="Reordered the temporary preview only. Stored IMG animation order is unchanged.";
}
