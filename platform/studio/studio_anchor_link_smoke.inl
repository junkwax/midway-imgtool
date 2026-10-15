struct AnchorLinkSmoke {
    int failures=0;uint64_t reference_doc=0,target_doc=0,generation=0;int old_x=0,new_x=0;std::string saved;
    void prepare(App &app,ImGuiIO &io,int frame){auto &u=app.anchor_link;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Anchor link UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        if(frame==0){io.AddFocusEvent(true);reference_doc=app.tab()->uid;generation=app.tab()->art->generation();app.demo();target_doc=app.tab()->uid;auto t=app.tab();saved=(fs::u8path(app.prefs)/"linked-target.IMG").u8string();check(t->art->save(saved,app.error));old_x=(short)t->art->sprite(0)->anix;
            app.show_anchor_link(*t);u.reference_doc=reference_doc;u.reference_id=app.find(reference_doc)->art->sprite_id(0);u.target_doc=target_doc;u.target_id=t->art->sprite_id(0);u.origin_y=120;u.options.offset_x=70;}
        auto ref=app.find(reference_doc),dst=app.find(target_doc);if(!ref||!dst){++failures;return;}
        if(frame==3)point(u.reference_point);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.reference_picked&&!u.target_picked);point(u.target_point);}if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(u.valid&&u.plan.changed&&!dst->art->dirty());new_x=u.plan.x;check(new_x==old_x+70);point(u.apply_point);}if(frame==12)io.AddMouseButtonEvent(0,true);if(frame==13)io.AddMouseButtonEvent(0,false);
        if(frame==15){check(!u.valid&&dst->art->dirty()&&(short)dst->art->sprite(0)->anix==new_x&&ref->art->generation()==generation);point(u.undo_point);}if(frame==16)io.AddMouseButtonEvent(0,true);if(frame==17)io.AddMouseButtonEvent(0,false);
        if(frame==19){check(!dst->art->dirty()&&(short)dst->art->sprite(0)->anix==old_x);point(u.redo_point);}if(frame==20)io.AddMouseButtonEvent(0,true);if(frame==21)io.AddMouseButtonEvent(0,false);
        if(frame==23){check(dst->art->dirty()&&(short)dst->art->sprite(0)->anix==new_x);check(app.save(*dst,false));auto loaded=Artwork::open(saved,app.error);check(loaded&&(short)loaded->sprite(0)->anix==new_x);reset(app);}
        if(frame==26)point(u.reference_point);if(frame==27)io.AddMouseButtonEvent(0,true);if(frame==28)io.AddMouseButtonEvent(0,false);
        if(frame==30){check(u.reference_picked);point(u.target_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(u.valid);ref->art->anchor(0,23,61);ref->art->commit();}
        if(frame==36){check(!u.valid&&!u.reference_picked&&!u.error.empty());ref->art->undo();check(!ref->art->dirty());point(u.reset_point);}if(frame==37)io.AddKeyEvent(ImGuiKey_Escape,true);if(frame==38)io.AddKeyEvent(ImGuiKey_Escape,false);
        if(frame==40){check(u.error.empty());u.reference_doc=target_doc;u.reference_id=dst->art->sprite_id(1);u.target_id=dst->art->sprite_id(2);u.options.reference_flip=true;u.options.target_flip=true;u.options.offset_x=20;}
        if(frame==43)point(u.reference_point);if(frame==44)io.AddMouseButtonEvent(0,true);if(frame==45)io.AddMouseButtonEvent(0,false);
        if(frame==47)point(u.target_point);if(frame==48)io.AddMouseButtonEvent(0,true);if(frame==49)io.AddMouseButtonEvent(0,false);
        if(frame==51){check(u.valid&&u.options.target_flip&&!dst->art->dirty());point(u.close_point);}if(frame==52)io.AddMouseButtonEvent(0,true);if(frame==53)io.AddMouseButtonEvent(0,false);
        if(frame==55){check(!u.open&&!u.valid&&!dst->art->dirty());app.show_anchor_link(*dst);u.reference_doc=reference_doc;u.reference_id=ref->art->sprite_id(0);u.target_id=dst->art->sprite_id(0);u.origin_y=120;u.options.offset_x=70;}
    }
    void reset(App &app){app.reset_anchor_link();}
};
