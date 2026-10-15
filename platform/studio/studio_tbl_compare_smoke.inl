struct TableCompareSmoke {
    int failures=0;uint64_t generation=0;std::string path;
    void prepare(App &app,ImGuiIO &io,int frame){auto t=app.tab();if(!t){++failures;return;}auto &u=app.table_compare;
        auto check=[&](bool ok){if(!ok){++failures;fprintf(stderr,"TBL compare UI failure at %d: %s / %s\n",frame,app.error.c_str(),u.error.c_str());}};auto point=[&](ImVec2 p){io.AddMousePosEvent(std::round(p.x),std::round(p.y));};
        if(frame==0){io.AddFocusEvent(true);generation=t->art->generation();path=(fs::u8path(app.prefs)/"comparison.TBL").u8string();std::ofstream out(fs::u8path(path));out<<"SENTINEL1:\n.word 48,64,24,60\n.long STUDIOPAL\nSENTINEL2:\n.word 48,64,23,59\nMISSING:\n.word 1,1,0,0\n";out.close();app.show_table_compare(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());}
        if(frame==3)point(u.compare_point);if(frame==4)io.AddMouseButtonEvent(0,true);if(frame==5)io.AddMouseButtonEvent(0,false);
        if(frame==7){check(u.valid&&u.report.summary.matched==1&&u.report.summary.differing==1&&u.report.summary.only_in_tbl==1&&u.report.summary.only_in_img==6&&t->art->generation()==generation);point(u.row_points[2]);}
        if(frame==8)io.AddMouseButtonEvent(0,true);if(frame==9)io.AddMouseButtonEvent(0,false);
        if(frame==11){check(u.selected==3&&t->selected==2&&!t->art->dirty());t->art->anchor(1,23,59);t->art->commit();}
        if(frame==14){check(u.valid&&u.report.generation!=t->art->generation());point(u.compare_point);}if(frame==15)io.AddMouseButtonEvent(0,true);if(frame==16)io.AddMouseButtonEvent(0,false);
        if(frame==18){check(u.valid&&u.report.summary.matched==2&&u.report.summary.differing==0);t->art->undo();std::ofstream out(fs::u8path(path),std::ios::app);out<<"SENTINEL1:\n.word 48,64,24,60\n";out.close();point(u.compare_point);}
        if(frame==19)io.AddMouseButtonEvent(0,true);if(frame==20)io.AddMouseButtonEvent(0,false);
        if(frame==22){check(!u.valid&&u.error.find("Duplicate")!=std::string::npos&&!t->art->dirty());point(u.close_point);}if(frame==23)io.AddMouseButtonEvent(0,true);if(frame==24)io.AddMouseButtonEvent(0,false);
        if(frame==26){check(!u.open);std::ofstream out(fs::u8path(path));out<<"SENTINEL1:\n.word 48,64,24,60\n.long STUDIOPAL\nSENTINEL2:\n.word 48,64,23,59\n";out.close();app.show_table_compare(*t);snprintf(u.path,sizeof u.path,"%s",path.c_str());app.rebuild_table_compare();}
    }
};
