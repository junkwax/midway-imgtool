struct FamilySmoke {
    int failures=0;uint64_t source_uid=0,target_uid=0;std::vector<uint64_t> ids;
    void prepare(App &app,ImGuiIO &io,int frame){auto source=frame?app.find(source_uid):app.tab();if(!source){++failures;return;}
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Family UI failure at %d: %s / copy=%s / delete=%s\n",frame,app.error.c_str(),app.library_copy.error.c_str(),app.deleting.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};auto click=[&](int at,ImVec2 p){if(frame==at){point(p);io.AddMouseButtonEvent(0,true);}if(frame==at+1)io.AddMouseButtonEvent(0,false);};
        auto activate=[&](uint64_t uid){for(int i=0;i<(int)app.tabs.size();++i)if(app.tabs[i]->uid==uid){app.active=i;app.select_uid=uid;}};
        if(frame==0){io.AddFocusEvent(true);source_uid=source->uid;const char *names[]={"HERO1","HERO1A","HERO1B","HERO1A_1","HERO1FLIP","HERO1C","HERO10","KEEP"};for(int i=0;i<source->art->count();++i){ids.push_back(source->art->sprite_id(i));check(source->art->rename_sprite(i,names[i],app.error));memset(source->art->sprite(i)->src_filename,0,16);}memcpy(source->art->sprite(5)->src_filename,"OTHER",6);check(source->art->embedded_add_record(false,"KEEP_CHILD",app.error)==0&&source->art->embedded_append(0,2,app.error));app.mark_sprite(*source,ids[0],true);app.new_library();target_uid=app.tab()->uid;activate(source_uid);app.action(Action::LibraryCopy);}
        auto target=app.find(target_uid);if(!target){++failures;return;}auto &c=app.library_copy;auto &d=app.deleting;
        if(frame==3)check(c.valid&&c.plan.rows.size()==1&&!c.options.include_children);click(4,c.children_point);
        if(frame==8)check(c.valid&&c.plan.rows.size()==3&&c.plan.scope.rows[1].parent==ids[0]&&c.plan.scope.rows[2].parent==ids[0]);click(9,c.row_points[2]);
        if(frame==13)check(!c.plan.rows[2].enabled&&c.options.excluded.count(ids[2]));click(14,c.children_point);if(frame==18)check(c.plan.rows.size()==1);click(19,c.children_point);
        if(frame==23)check(c.plan.rows.size()==3&&!c.plan.rows[2].enabled);click(24,c.apply_point);
        if(frame==28){check(!c.open&&target->art->count()==3&&std::string(target->art->sprite(2)->n_s)=="HERO1A"&&source->art->count()==8);target->art->undo();check(target->art->count()==1);target->art->redo();check(target->art->count()==3);activate(source_uid);app.action(Action::BatchDelete);}
        if(frame==32)check(d.valid&&d.plan.rows.size()==1&&!d.include_children);click(33,d.children_point);if(frame==37)check(d.valid&&d.plan.rows.size()==3&&!d.plan.rows[2].references.empty());click(38,d.apply_point);
        if(frame==42)check(d.open&&source->art->count()==8);click(43,d.exclude_point);if(frame==47)check(!d.plan.rows[2].enabled&&d.plan.rows[1].enabled);click(48,d.children_point);if(frame==52)check(d.plan.rows.size()==1);click(53,d.children_point);
        if(frame==57)check(d.plan.rows.size()==3&&!d.plan.rows[2].enabled&&d.plan.scope.rows[2].parent==ids[0]);click(58,d.apply_point);
        if(frame==62){check(!d.open&&source->art->count()==6&&source->art->sprite_index(ids[2])==0&&source->art->sprite_index(ids[3])>=0&&source->art->sprite_index(ids[5])>=0);source->art->undo();check(source->art->count()==8&&source->art->sprite_id(0)==ids[0]);source->art->redo();check(source->art->count()==6);source->art->undo();app.action(Action::BatchDelete);}
        click(66,d.children_point);if(frame==70){check(d.valid&&d.plan.rows.size()==3);d.selected=1;d.preview_dirty=true;point(ImVec2(5,5));}
    }
};
