struct PointsSmoke {
    int failures=0;uint64_t generation=0;SpritePoints expected{};
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.points;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"Points UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};
        auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        if(frame==0){io.AddFocusEvent(true);generation=t->art->generation();app.show_points(*t);}
        if(frame==3){check(u.valid&&!u.edit.enabled);point(u.enable_point);}if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.edit.enabled&&t->art->generation()==generation&&!t->art->sprite(0)->pttbl_p);point(u.preview_origin+ImVec2(12*u.preview_zoom,20*u.preview_zoom));}
        if(frame==8)io.AddMouseButtonEvent(1,true);if(frame==9)io.AddMouseButtonEvent(1,false);
        if(frame==11){check(u.edit.points[0]==12&&u.edit.points[1]==20);point(u.handles[0]);}if(frame==12)io.AddMouseButtonEvent(0,true);
        if(frame==13)point(u.preview_origin+ImVec2(20*u.preview_zoom,30*u.preview_zoom));if(frame==14)io.AddMouseButtonEvent(0,false);
        if(frame==16){check(u.edit.points[0]==20&&u.edit.points[1]==30&&!t->art->dirty());point(u.cancel_point);}if(frame==17)io.AddMouseButtonEvent(0,true);if(frame==18)io.AddMouseButtonEvent(0,false);
        if(frame==20){check(!u.open&&t->art->generation()==generation&&!t->art->dirty());app.show_points(*t);u.edit.enabled=true;for(int i=0;i<20;++i)u.edit.points[i]=i%2?20+i:i*2;expected=u.edit.points;}
        if(frame==23)point(u.apply_point);if(frame==24)io.AddMouseButtonEvent(0,true);if(frame==25)io.AddMouseButtonEvent(0,false);
        if(frame==27){PointTableEdit read;check(!u.open&&t->art->dirty()&&read_point_table(*t->art,t->selected_id,read,app.error)&&read.points==expected);app.action(Action::Undo);check(!t->art->dirty()&&!t->art->sprite(0)->pttbl_p);app.action(Action::Redo);check(t->art->sprite(0)->pttbl_p);app.show_points(*t);}
        if(frame==30){u.edit.enabled=false;point(u.apply_point);}if(frame==31)io.AddMouseButtonEvent(0,true);if(frame==32)io.AddMouseButtonEvent(0,false);
        if(frame==34){check(!u.open&&!t->art->sprite(0)->pttbl_p);app.action(Action::Undo);check(t->art->sprite(0)->pttbl_p);app.show_points(*t);}
        if(frame==37){t->art->anchor(0,25,61);t->art->commit();point(u.apply_point);}if(frame==38)io.AddMouseButtonEvent(0,true);if(frame==39)io.AddMouseButtonEvent(0,false);
        if(frame==41){check(u.open&&u.edit.generation!=t->art->generation());point(u.reload_point);}if(frame==42)io.AddMouseButtonEvent(0,true);if(frame==43)io.AddMouseButtonEvent(0,false);
        if(frame==45){check(u.valid&&u.edit.generation==t->art->generation()&&u.edit.enabled&&u.edit.points==expected);point(u.cancel_point);}if(frame==46)io.AddMouseButtonEvent(0,true);if(frame==47)io.AddMouseButtonEvent(0,false);
        if(frame==49){check(!u.open);app.action(Action::Undo);app.action(Action::Undo);check(!t->art->dirty());app.show_points(*t);u.edit.enabled=true;for(int i=0;i<10;++i){u.edit.points[i*2]=i<5?6+i*8:38-(i-5)*8;u.edit.points[i*2+1]=i<5?12:52;}u.polygon=true;}
    }
};
