#include "cloudgenshin_stream_view.hpp"
#include "app_paths.hpp"
#include "mihoyo/stream_health_draw.hpp"
#include "mihoyo/compact_stream_menu_draw.hpp"
#include <cstdio>

namespace ui=cloudgenshin::ui;
namespace overlay=cloudgenshin::overlay;

void CloudGenshinStreamView::toggle_language() {
    ui::chinese=!ui::chinese;texts_.refresh();
    opennow::PrepareAppStorage();
    progress_(ui::SaveLanguage(opennow::AppHomePath()+"/cloudgenshin-language.txt")
        ? "LANGUAGE_SAVED" : "LANGUAGE_SAVE_FAILED",0,0);
    diagnostics_refresh_={};
}
void CloudGenshinStreamView::arm_input_release() {
    input_release_pending_=true;
    release_gate_.arm(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    // A capture may open and close between network polls. Its generation
    // forces an accepted neutral even when the worker never sampled the panel.
    std::lock_guard lock(mutex_);
    input_={};motion_input_={};++controller_input_generation_;
}
void CloudGenshinStreamView::request_continue() {
    if(!finished_ && !exit_requested_) {
        continue_requested_=true;arm_input_release();
        progress_("INPUT_CONTINUE_REQUESTED_WAITING_READY",0,0);
    }
}
void CloudGenshinStreamView::show_quality_menu(bool visible) {
    if(visible && exit_requested_) return;
    if(visible && diagnostics_visible_) show_diagnostics(false);
    if(visible==quality_menu_open_) return;
    quality_menu_open_=visible;confirm_exit_=false;menu_feedback_.clear();
    menu_tap_.cancel();floating_ball_.cancel();arm_input_release();
    {std::lock_guard lock(mutex_);input_={};}
    if(visible) {
        // Home opens a submenu without changing the restored preference.
        stream_menu_.reset();quality_menu_index_=0;
        brls::Application::giveFocus(this);
    }
    progress_(visible ? "QUALITY_MENU_OPENED" : "QUALITY_MENU_CLOSED",0,0);
}
void CloudGenshinStreamView::PersistStreamSettings() {
    // Only the last successfully sent request counts; a failed send keeps the
    // previous saved value so the next session retries the known-good setting.
    const int graphics=accepted_graphics_.value_or(persisted_graphics_);
    const int fps=accepted_fps_.value_or(persisted_fps_);
    // With no saved pair, wait until both startup requests have succeeded.
    // A success for graphics must not persist a queued/failed FPS selection.
    if(!overlay::ValidGraphics(graphics) || !overlay::ValidFps(fps)) return;
    if(graphics==persisted_graphics_ && fps==persisted_fps_) return;
    if(!cloudgenshin::overlay::SaveSavedSettings(
           overlay::StreamSettingsPath(opennow::AppHomePath(),mapping_profile_),graphics,fps)) {
        // The request may have reached the pod, but the next launch would fall
        // back to the startup preset. Keep the old snapshot and say so.
        progress_("STREAM_SETTINGS_SAVE_FAILED",graphics*100+fps,0);
        return;
    }
    persisted_graphics_=graphics;persisted_fps_=fps;
    progress_("STREAM_SETTINGS_SAVED",graphics*100+fps,0);
}
void CloudGenshinStreamView::activate_quality_row(int row) {
    if(row<0 || row>=stream_menu_.count()) return;
    quality_menu_index_=row;
    const auto selection=stream_menu_.select(row,graphics_request_.snapshot().value(),fps_request_.snapshot().value());
    if(selection.navigation) {
        quality_menu_index_=selection.focus;confirm_exit_=false;menu_feedback_.clear();
        menu_tap_.cancel();arm_input_release();return;
    }
    row=selection.command;
    if(row!=10 && confirm_exit_) {confirm_exit_=false;menu_feedback_.clear();}
    if(row<=4) {
        if(!settings_ready_ || finished_ || exit_requested_) {menu_feedback_="Settings unavailable until game auth";return;}
        if(row<=2) {graphics_request_.request(row);progress_("STREAM_QUALITY_REQUESTED",row,0);}
        else {const int fps=row==3 ? 30 : 60;fps_request_.request(fps);progress_("STREAM_FPS_REQUESTED",fps,0);}
        menu_feedback_="Request queued; check actual stream.";
    } else if(row==5) stats_visible_=!stats_visible_;
    else if(row==6) toggle_language();
    else if(row==7) show_diagnostics(true);
    else if(row==8) {
        if(mapping_profile_!="hk4e") return;
        cloudgenshin::input::GyroSettings candidate;
        {std::lock_guard lock(mutex_);candidate=gyro_settings_;}
        candidate.gyro_assist=!candidate.gyro_assist;
        opennow::PrepareAppStorage();
        const bool saved=cloudgenshin::input::SaveGyroSettings(opennow::AppHomePath(),candidate);
        if(saved) {std::lock_guard lock(mutex_);gyro_settings_=candidate;}
        menu_feedback_=saved ? (candidate.gyro_assist?"Gyro aim enabled (hold ZL)":"Gyro aim disabled") : "SD failed / photograph log";
        progress_(saved?"GYRO_SETTINGS_SAVED":"GYRO_SETTINGS_SAVE_FAILED",candidate.gyro_assist?1:0,0);
        arm_input_release();
    } else if(row==9) {
        if(!control_ready_ || media_.decoded()==0 || finished_) {menu_feedback_="Continue needs video and control ready";return;}
        request_continue();show_quality_menu(false);
    } else if(row==10) {
        if(!confirm_exit_) {confirm_exit_=true;menu_feedback_="Select stop again to confirm. B cancels.";}
        else request_exit();
    } else if(row==11) {
        const bool saved=decoder_selection_.toggle();
        menu_feedback_=saved ? "Decoder saved; stop and start stream to apply" : "SD failed / photograph log";
        progress_(saved ? "VIDEO_DECODER_PREFERENCE_SAVED" : "VIDEO_DECODER_PREFERENCE_SAVE_FAILED",decoder_selection_.preferred()?1:0,0);
    }
}
void CloudGenshinStreamView::update_overlay_input(const cloudgenshin::input::SwitchControllerState& state,
    overlay::Rect bounds,bool touch_down,int finger,overlay::Point point) {
    const bool was_open=quality_menu_open_;
    bool touch_committed=false;
    if(diagnostics_visible_ || exit_requested_) {
        floating_ball_.update(touch_down,finger,point,bounds);floating_ball_.cancel();
        menu_tap_.cancel();
    } else if(quality_menu_open_) {
        const overlay::CompactStreamMenuLayout layout(bounds,stream_menu_.count());
        const int target=layout.close.contains(point) || !layout.panel.contains(point) ? -2 : layout.hit_row(point);
        if(const auto tap=menu_tap_.update(touch_down,finger,target)) {
            touch_committed=true;
            if(*tap==-2) show_quality_menu(false);else activate_quality_row(*tap);
        }
    } else {
        if(floating_ball_.update(touch_down,finger,point,bounds)) show_quality_menu(true);
        if(floating_ball_.captured() && !ball_was_captured_) arm_input_release();
        ball_was_captured_=floating_ball_.captured();
        menu_tap_.update(touch_down,finger,-1);
    }
    const auto now_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool can_open=mapping_profile_!="hk4e" &&
        !diagnostics_visible_ && !exit_requested_ && !input_release_pending_;
    const auto chord=quality_chord_.update(state.l && can_open,state.r && can_open,now_ms);
    if(quality_chord_captured_ && !chord.capture) arm_input_release();
    quality_chord_captured_=chord.capture;
    // The chord owns its hold window; do not reset it merely because it captured input.
    if(chord.open && !quality_menu_open_ && !diagnostics_visible_) show_quality_menu(true);
    // A tap and a controller press in the same frame are one UI interaction,
    // not two independent activations of the destructive stop confirmation.
    if(was_open && quality_menu_open_ && !input_release_pending_ && !touch_committed) {
        const bool moved=(state.dpad_up && !quality_menu_up_) || (state.dpad_down && !quality_menu_down_);
        if(moved && confirm_exit_) {confirm_exit_=false;menu_feedback_.clear();}
        if(state.dpad_up && !quality_menu_up_) quality_menu_index_=(quality_menu_index_+stream_menu_.count()-1)%stream_menu_.count();
        if(state.dpad_down && !quality_menu_down_) quality_menu_index_=(quality_menu_index_+1)%stream_menu_.count();
        if(state.b && !quality_menu_b_) {
            if(confirm_exit_) {confirm_exit_=false;menu_feedback_.clear();}
            else if(const auto focus=stream_menu_.back()) {
                quality_menu_index_=*focus;menu_feedback_.clear();menu_tap_.cancel();arm_input_release();
            } else show_quality_menu(false);
        } else if(state.a && !quality_menu_a_) activate_quality_row(quality_menu_index_);
    }
    quality_menu_up_=state.dpad_up;quality_menu_down_=state.dpad_down;
    quality_menu_a_=state.a;quality_menu_b_=state.b;
}

void CloudGenshinStreamView::draw_overlay(NVGcontext* vg,overlay::Rect bounds) {
    if(diagnostics_visible_ || exit_requested_) return;
    nvgSave(vg);nvgIntersectScissor(vg,bounds.x,bounds.y,bounds.w,bounds.h);
    nvgFontFaceId(vg,brls::Application::getFont(ui::chinese ? brls::FONT_CHINESE_SIMPLIFIED : brls::FONT_REGULAR));
    const auto cyan=nvgRGB(90,225,235);
    std::string resolution;
    if(stats_visible_ || quality_menu_open_) {
        resolution=frame_metadata_seen_
            ? std::to_string(frame_width_.load())+" x "+std::to_string(frame_height_.load()) : "-- x --";
    }
    if(stats_visible_ && !quality_menu_open_) {
        overlay::DrawStreamHealth(vg,bounds.x+20,bounds.y+18,resolution,hardware_decoder_,stream_health_.window());
    }
    if(!quality_menu_open_) {
        const auto c=floating_ball_.centre(bounds);
        nvgBeginPath(vg);nvgCircle(vg,c.x,c.y,29);nvgFillColor(vg,nvgRGBA(12,28,44,215));nvgFill(vg);
        nvgStrokeColor(vg,cyan);nvgStrokeWidth(vg,2);nvgStroke(vg);
        // Vector sliders remain legible without special icon glyphs.
        for(int i=0;i<3;++i) {
            const float yy=c.y-10+i*10,xx=c.x+(i==1 ? 7 : -6);
            nvgBeginPath(vg);nvgMoveTo(vg,c.x-14,yy);nvgLineTo(vg,c.x+14,yy);
            nvgStrokeWidth(vg,2);nvgStrokeColor(vg,cyan);nvgStroke(vg);
            nvgBeginPath(vg);nvgCircle(vg,xx,yy,3);nvgFillColor(vg,cyan);nvgFill(vg);
        }
        nvgRestore(vg);return;
    }
    overlay::CompactStreamMenuStatus status;
    status.resolution=resolution;status.feedback=menu_feedback_;
    status.decoded_fps=stream_rates_.fps();status.mbps=stream_rates_.mbps();
    status.hardware=hardware_decoder_;status.settings_ready=settings_ready_ && !finished_;
    status.control_ready=control_ready_ && !finished_;
    status.continue_ready=status.control_ready && media_.decoded()>0;
    status.hud=stats_visible_;status.hardware_preferred=decoder_selection_.preferred();status.confirm_exit=confirm_exit_;
    status.quality=graphics_request_.snapshot();status.fps=fps_request_.snapshot();
    status.smoothness_protection=smoothness_protection_;status.active_fps=active_fps_;
    {std::lock_guard lock(mutex_);status.gyro_enabled=gyro_settings_.gyro_assist;}
    overlay::DrawCompactStreamMenu(vg,bounds,stream_menu_,quality_menu_index_,status);
    nvgRestore(vg);
}
