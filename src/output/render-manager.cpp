#include "wayfire/render-manager.hpp"
#include "pixman.h"
#include "wayfire/config-backend.hpp"
#include "wayfire/core.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/region.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/view.hpp"
#include "wayfire/output.hpp"
#include "wayfire/util.hpp"
#include "../main.hpp"
#include "wayfire/workspace-set.hpp" // IWYU pragma: keep
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <wayfire/nonstd/reverse.hpp>
#include <wayfire/nonstd/safe-list.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wayfire/output-layout.hpp>

namespace wf
{
/**
 * swapchain_damage_manager_t is responsible for tracking the damage and managing the swapchain on the
 * given output.
 */
struct swapchain_damage_manager_t
{
    wf::option_wrapper_t<bool> force_frame_sync{"workarounds/force_frame_sync"};
    signal::connection_t<scene::root_node_update_signal> root_update;
    std::vector<scene::render_instance_uptr> render_instances;

    wf::wl_listener_wrapper on_needs_frame;
    wf::wl_listener_wrapper on_damage;
    wf::wl_listener_wrapper on_request_state;
    wf::wl_listener_wrapper on_gamma_changed;

    wf::region_t frame_damage;
    wlr_output *output;
    wlr_damage_ring damage_ring;
    output_t *wo;

    bool pending_gamma_lut = false;
    wf::wl_idle_call idle_recompute_visibility;

    void update_scenegraph(uint32_t update_mask)
    {
        if (update_mask & scene::update_flag::MASKED)
        {
            return;
        }

        constexpr uint32_t recompute_instances_on = scene::update_flag::CHILDREN_LIST |
            scene::update_flag::ENABLED;
        constexpr uint32_t recompute_visibility_on = recompute_instances_on | scene::update_flag::GEOMETRY;

        if (update_mask & recompute_instances_on)
        {
            LOGC(RENDER, "Output ", wo->to_string(), ": regenerating instances.");
            auto root = wf::get_core().scene();
            scene::damage_callback push_damage = [=] (wf::region_t region)
            {
                // Damage is pushed up to the root in root coordinate system,
                // we need it in layout-local coordinate system.
                region += -wf::origin(wo->get_layout_geometry());
                this->damage(region, true);
            };

            render_instances.clear();
            root->gen_render_instances(render_instances, push_damage, wo);
        }

        if (update_mask & recompute_visibility_on)
        {
            idle_recompute_visibility.run_once([=] ()
            {
                LOGC(RENDER, "Output ", wo->to_string(), ": recomputing visibility.");
                wf::region_t region = this->wo->get_layout_geometry();
                for (auto& inst : render_instances)
                {
                    inst->compute_visibility(wo, region);
                }
            });
        }
    }

    void update_damage_ring_bounds()
    {
        int width, height;
        wlr_output_transformed_resolution(output, &width, &height);
        wlr_damage_ring_set_bounds(&damage_ring, width, height);
    }

    void start_rendering()
    {
        auto root = wf::get_core().scene();
        root_update = [=] (scene::root_node_update_signal *data)
        {
            update_scenegraph(data->flags);
        };

        root->connect<scene::root_node_update_signal>(&root_update);
        update_scenegraph(scene::update_flag::CHILDREN_LIST);
    }

    swapchain_damage_manager_t(output_t *output)
    {
        this->output = output->handle;
        this->wo     = output;

        output->connect(&output_mode_changed);

        wlr_damage_ring_init(&damage_ring);
        update_damage_ring_bounds();

        on_needs_frame.set_callback([=] (void*) { schedule_repaint(); });
        on_damage.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_output_event_damage*>(data);
            if (wlr_damage_ring_add(&damage_ring, ev->damage))
            {
                schedule_repaint();
            }
        });

        on_request_state.set_callback([=] (void *data)
        {
            auto ev = static_cast<wlr_output_event_request_state*>(data);
            wlr_output_commit_state(output->handle, ev->state);
            update_damage_ring_bounds();
            damage_whole();
            schedule_repaint();
        });

        on_gamma_changed.set_callback([=] (void *data)
        {
            auto event = (const wlr_gamma_control_manager_v1_set_gamma_event*)data;
            if (event->output == this->output)
            {
                pending_gamma_lut = true;
                schedule_repaint();
            }
        });

        on_needs_frame.connect(&output->handle->events.needs_frame);
        on_damage.connect(&output->handle->events.damage);
        on_request_state.connect(&output->handle->events.request_state);
        on_gamma_changed.connect(&wf::get_core().protocols.gamma_v1->events.set_gamma);
    }

    wf::signal::connection_t<wf::output_configuration_changed_signal>
    output_mode_changed = [=] (wf::output_configuration_changed_signal *ev)
    {
        if (!ev || !ev->changed_fields)
        {
            return;
        }

        update_damage_ring_bounds();
        schedule_repaint();
    };

    /**
     * Damage the given region
     */
    void damage(const wf::region_t& region, bool repaint)
    {
        if (region.empty())
        {
            return;
        }

        /* Wlroots expects damage after scaling */
        auto scaled_region = region * wo->handle->scale;
        frame_damage |= scaled_region;
        wlr_damage_ring_add(&damage_ring, scaled_region.to_pixman());
        if (repaint)
        {
            schedule_repaint();
        }
    }

    void damage(const wf::geometry_t& box, bool repaint)
    {
        if ((box.width <= 0) || (box.height <= 0))
        {
            return;
        }

        /* Wlroots expects damage after scaling */
        auto scaled_box = box * wo->handle->scale;
        frame_damage |= scaled_box;
        wlr_damage_ring_add_box(&damage_ring, &scaled_box);
        if (repaint)
        {
            schedule_repaint();
        }
    }

    int constant_redraw_counter = 0;
    void set_redraw_always(bool always)
    {
        constant_redraw_counter += (always ? 1 : -1);
        if (constant_redraw_counter > 1) /* no change, exit */
        {
            return;
        }

        if (constant_redraw_counter < 0)
        {
            LOGE("constant_redraw_counter got below 0!");
            constant_redraw_counter = 0;

            return;
        }

        schedule_repaint();
    }

    wf::region_t acc_damage;

    // A struct which contains the necessary structures for painting one frame
    struct frame_object_t
    {
        wlr_output_state state;
        wlr_buffer *buffer = NULL;
        int buffer_age;

        frame_object_t()
        {
            wlr_output_state_init(&state);
        }

        ~frame_object_t()
        {
            wlr_output_state_finish(&state);
        }

        frame_object_t(const frame_object_t&) = delete;
        frame_object_t(frame_object_t&&) = delete;
        frame_object_t& operator =(const frame_object_t&) = delete;
        frame_object_t& operator =(frame_object_t&&) = delete;
    };

    bool acquire_next_swapchain_buffer(frame_object_t& frame)
    {
        int width, height;
        wlr_output_transformed_resolution(output, &width, &height);
        wlr_region_transform(&frame.state.damage, &damage_ring.current,
            wlr_output_transform_invert(output->transform), width, height);

        if (!wlr_output_configure_primary_swapchain(output, &frame.state, &output->swapchain))
        {
            LOGE("Failed to configure primary output swapchain for output ", nonull(output->name));
            return false;
        }

        frame.buffer = wlr_swapchain_acquire(output->swapchain, &frame.buffer_age);
        if (!frame.buffer)
        {
            LOGE("Failed to acquire buffer from the output swapchain!");
            return false;
        }

        return true;
    }

    bool try_apply_gamma(frame_object_t& next_frame)
    {
        if (!pending_gamma_lut)
        {
            return true;
        }

        pending_gamma_lut = false;
        auto gamma_control =
            wlr_gamma_control_manager_v1_get_control(wf::get_core().protocols.gamma_v1, output);

        if (!wlr_gamma_control_v1_apply(gamma_control, &next_frame.state))
        {
            LOGE("Failed to apply gamma to output state!");
            return false;
        }

        if (!wlr_output_test_state(output, &next_frame.state))
        {
            wlr_gamma_control_v1_send_failed_and_destroy(gamma_control);
        }

        return true;
    }

    bool force_next_frame = false;
    /**
     * Start rendering a new frame.
     * If the operation could not be started, or if a new frame is not needed, the function returns false.
     * If the operation succeeds, true is returned, and the output (E)GL context is bound.
     */
    std::unique_ptr<frame_object_t> start_frame()
    {
        const bool needs_swap = force_next_frame | output->needs_frame |
            pixman_region32_not_empty(&damage_ring.current) | (constant_redraw_counter > 0);
        force_next_frame = false;

        if (!needs_swap)
        {
            return {};
        }

        auto next_frame = std::make_unique<frame_object_t>();
        next_frame->state.committed |= WLR_OUTPUT_STATE_DAMAGE;

        if (!try_apply_gamma(*next_frame))
        {
            return {};
        }

        if (!acquire_next_swapchain_buffer(*next_frame))
        {
            return {};
        }

        // Accumulate damage now, when we are sure we will render the frame.
        // Doing this earlier may mean that the damage from the previous frames
        // creeps into the current frame damage, if we had skipped a frame.
        accumulate_damage(next_frame->buffer_age);

        return next_frame;
    }

    void swap_buffers(std::unique_ptr<frame_object_t> next_frame, wf::render_pass_t pass,
        const wf::region_t& swap_damage)
    {
        /* If force frame sync option is set, call glFinish to block until
         * the GPU finishes rendering. This can work around some driver
         * bugs, but may cause more resource usage. */
        if (force_frame_sync)
        {
            wf::gles::run_in_context_if_gles([&]
            {
                GL_CALL(glFinish());
            });
        }

        frame_damage.clear();
        if (!pass.submit())
        {
            LOGE("Failed to submit render pass!");
            wlr_buffer_unlock(next_frame->buffer);
            return;
        }

        wlr_output_state_set_buffer(&next_frame->state, next_frame->buffer);
        wlr_buffer_unlock(next_frame->buffer);

        if (!wlr_output_test_state(output, &next_frame->state))
        {
            LOGE("Output test failed!");
            return;
        }

        if (!wlr_output_commit_state(output, &next_frame->state))
        {
            LOGE("Output commit failed!");
            return;
        }

        wlr_damage_ring_rotate(&damage_ring);
    }

    /**
     * Accumulate damage from last frame.
     * Needs to be called after make_current()
     */
    void accumulate_damage(int buffer_age)
    {
        wlr_damage_ring_get_buffer_damage(&damage_ring, buffer_age, acc_damage.to_pixman());
        frame_damage |= acc_damage;
        if (runtime_config.no_damage_track)
        {
            frame_damage |= get_wlr_damage_box();
        }
    }

    /**
     * Return the damage that has been scheduled for the next frame up to now,
     * or, if in a repaint, the damage for the current frame
     */
    wf::region_t get_scheduled_damage()
    {
        return frame_damage * (1.0 / wo->handle->scale);
    }

    /**
     * Schedule a frame for the output
     */
    void schedule_repaint()
    {
        wlr_output_schedule_frame(output);
        force_next_frame = true;
    }

    /**
     * Return the extents of the visible region for the output in the wlroots
     * damage coordinate system.
     */
    wlr_box get_wlr_damage_box() const
    {
        int w, h;
        wlr_output_transformed_resolution(output, &w, &h);

        return {0, 0, w, h};
    }

    /**
     * Same as render_manager::get_ws_box()
     */
    wlr_box get_ws_box(wf::point_t ws) const
    {
        auto current = wo->wset()->get_current_workspace();

        wlr_box box = wo->get_relative_geometry();
        box.x = (ws.x - current.x) * box.width;
        box.y = (ws.y - current.y) * box.height;

        return box;
    }

    /**
     * Returns the scheduled damage for the given workspace, in output-local
     * coordinates.
     */
    wf::region_t get_ws_damage(wf::point_t ws)
    {
        auto scaled = frame_damage * (1.0 / wo->handle->scale);

        return scaled & get_ws_box(ws);
    }

    /**
     * Same as render_manager::damage_whole()
     */
    void damage_whole()
    {
        auto vsize = wo->wset()->get_workspace_grid_size();
        auto vp    = wo->wset()->get_current_workspace();
        auto res   = wo->get_screen_size();

        damage(wf::geometry_t{
                -vp.x * res.width,
                -vp.y * res.height,
                vsize.width * res.width,
                vsize.height * res.height,
            }, true);
    }

    wf::wl_idle_call idle_damage;
    /**
     * Same as render_manager::damage_whole_idle()
     */
    void damage_whole_idle()
    {
        damage_whole();
        if (!idle_damage.is_connected())
        {
            idle_damage.run_once([&] () { damage_whole(); });
        }
    }
};

/**
 * Very simple class to manage effect hooks
 */
struct effect_hook_manager_t
{
    using effect_container_t = wf::safe_list_t<effect_hook_t*>;
    effect_container_t effects[OUTPUT_EFFECT_TOTAL];

    void add_effect(effect_hook_t *hook, output_effect_type_t type)
    {
        effects[type].push_back(hook);
    }

    bool can_scanout() const
    {
        return effects[OUTPUT_EFFECT_OVERLAY].size() == 0 &&
               effects[OUTPUT_EFFECT_POST].size() == 0;
    }

    void rem_effect(effect_hook_t *hook)
    {
        for (int i = 0; i < OUTPUT_EFFECT_TOTAL; i++)
        {
            effects[i].remove_all(hook);
        }
    }

    void run_effects(output_effect_type_t type)
    {
        effects[type].for_each([] (auto effect)
        { (*effect)(); });
    }
};

/**
 * A class to manage and run postprocessing effects
 */
struct postprocessing_manager_t
{
    using post_container_t = wf::safe_list_t<post_hook_t*>;
    post_container_t post_effects;
    wf::auxilliary_buffer_t post_buffers[2];
    /* Buffer to which other operations render to */
    static constexpr uint32_t default_out_buffer = 0;

    output_t *output;
    uint32_t output_width, output_height;
    postprocessing_manager_t(output_t *output)
    {
        this->output = output;
    }

    wf::render_buffer_t final_target;
    void set_current_buffer(wlr_buffer *buffer)
    {
        final_target = wf::render_buffer_t{
            buffer,
            wf::dimensions_t{output->handle->width, output->handle->height}
        };
    }

    void allocate(int width, int height)
    {
        if (post_effects.size() == 0)
        {
            return;
        }

        output_width  = width;
        output_height = height;
        for (auto& buffer : post_buffers)
        {
            buffer.allocate({width, height});
        }
    }

    void add_post(post_hook_t *hook)
    {
        post_effects.push_back(hook);
        output->render->damage_whole_idle();
    }

    void rem_post(post_hook_t *hook)
    {
        post_effects.remove_all(hook);
        output->render->damage_whole_idle();
    }

    /* Run all postprocessing effects, rendering to alternating buffers and
     * finally to the screen.
     *
     * NB: 2 buffers just aren't enough. We render to the zero buffer, and then
     * we alternately render to the second and the third. The reason: We track
     * damage. So, we need to keep the whole buffer each frame. */
    void run_post_effects()
    {
        int cur_idx = 0;
        post_effects.for_each([&] (auto post) -> void
        {
            int next_idx = 1 - cur_idx;
            wf::render_buffer_t dst_buffer = (post == post_effects.back() ?
                final_target : post_buffers[next_idx].get_renderbuffer());
            (*post)(post_buffers[cur_idx], dst_buffer);
            cur_idx = next_idx;
        });
    }

    wf::render_target_t get_target_framebuffer() const
    {
        wf::render_target_t fb{
            post_effects.size() > 0 ? post_buffers[default_out_buffer].get_renderbuffer() : final_target
        };

        fb.geometry     = output->get_relative_geometry();
        fb.wl_transform = output->handle->transform;
        fb.scale = output->handle->scale;

        return fb;
    }

    bool can_scanout() const
    {
        return post_effects.size() == 0;
    }
};

/**
 * Responsible for attaching depth buffers to framebuffers.
 * It keeps at most 3 depth buffers at any given time to conserve
 * resources.
 */
class depth_buffer_manager_t
{
  public:
    void ensure_depth_buffer(int fb, int width, int height)
    {
        /* If the backend doesn't have its own framebuffer, then the
         * framebuffer is created with a depth buffer. */
        if ((fb == 0) || (required_counter <= 0))
        {
            return;
        }

        attach_buffer(find_buffer(fb), fb, width, height);
    }

    void set_required(bool require)
    {
        required_counter += require ? 1 : -1;
        if (required_counter <= 0)
        {
            free_all_buffers();
        }
    }

    depth_buffer_manager_t() = default;

    ~depth_buffer_manager_t()
    {
        free_all_buffers();
    }

    depth_buffer_manager_t(const depth_buffer_manager_t &) = delete;
    depth_buffer_manager_t(depth_buffer_manager_t &&) = delete;
    depth_buffer_manager_t& operator =(const depth_buffer_manager_t&) = delete;
    depth_buffer_manager_t& operator =(depth_buffer_manager_t&&) = delete;

  private:
    static constexpr size_t MAX_BUFFERS = 3;
    int required_counter = 0;

    struct depth_buffer_t
    {
        GLuint tex = -1;
        int attached_to = -1;
        int width  = 0;
        int height = 0;

        int64_t last_used = 0;
    };

    void free_buffer(depth_buffer_t& buffer)
    {
        if (buffer.tex != (GLuint) - 1)
        {
            GL_CALL(glDeleteTextures(1, &buffer.tex));
        }
    }

    void free_all_buffers()
    {
        wf::gles::run_in_context_if_gles([&]
        {
            for (auto& b : buffers)
            {
                free_buffer(b);
            }
        });
    }

    void attach_buffer(depth_buffer_t& buffer, int fb, int width, int height)
    {
        if ((buffer.attached_to == fb) &&
            (buffer.width == width) &&
            (buffer.height == height))
        {
            return;
        }

        free_buffer(buffer);
        GL_CALL(glGenTextures(1, &buffer.tex));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, buffer.tex));
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
            width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL));
        buffer.width  = width;
        buffer.height = height;

        GL_CALL(glBindTexture(GL_TEXTURE_2D, buffer.tex));
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            GL_TEXTURE_2D, buffer.tex, 0));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

        buffer.attached_to = fb;
        buffer.last_used   = get_current_time();
    }

    depth_buffer_t& find_buffer(int fb)
    {
        for (auto& buffer : buffers)
        {
            if (buffer.attached_to == fb)
            {
                return buffer;
            }
        }

        /** New buffer? */
        if (buffers.size() < MAX_BUFFERS)
        {
            buffers.push_back(depth_buffer_t{});

            return buffers.back();
        }

        /** Evict oldest */
        auto oldest = &buffers.front();
        for (auto& buffer : buffers)
        {
            if (buffer.last_used < oldest->last_used)
            {
                oldest = &buffer;
            }
        }

        return *oldest;
    }

    std::vector<depth_buffer_t> buffers;
};

/**
 * A struct which manages the repaint delay.
 *
 * The repaint delay is a technique to potentially lower the input latency.
 *
 * It works by delaying Wayfire's repainting after getting the next frame event.
 * During this time the clients have time to update and submit their buffers.
 * If they manage this on time, the next frame will contain the already new
 * application contents, otherwise, the changes are visible after 1 more frame.
 *
 * The repaint delay however should be chosen so that Wayfire's own rendering
 * starts early enough for the next vblank, otherwise, the framerate will suffer.
 *
 * Calculating the maximal time Wayfire needs for rendering is very hard, and
 * and can change depending on active plugins, number of opened windows, etc.
 *
 * Thus, we need to dynamically guess this time based on the previous frames.
 * Currently, the following algorithm is implemented:
 *
 * Initially, the repaint delay is zero.
 *
 * If at some point Wayfire skips a frame, the delay is assumed too big and
 * reduced by `2^i`, where `i` is the amount of consecutive skipped frames.
 *
 * If Wayfire renders in time for `increase_window` milliseconds, then the
 * delay is increased by one. If the next frame is delayed, then
 * `increase_window` is doubled, otherwise, it is halved
 * (but it must stay between `MIN_INCREASE_WINDOW` and `MAX_INCREASE_WINDOW`).
 */
struct repaint_delay_manager_t
{
    repaint_delay_manager_t(wf::output_t *output)
    {
        on_present.set_callback([&] (void *data)
        {
            auto ev = static_cast<wlr_output_event_present*>(data);
            this->refresh_nsec = ev->refresh;
        });
        on_present.connect(&output->handle->events.present);
    }

    /**
     * The next frame will be skipped.
     */
    void skip_frame()
    {
        // Mark last frame as invalid, because we don't know how much time
        // will pass until next frame
        last_pageflip = -1;
    }

    /**
     * Starting a new frame.
     */
    void start_frame()
    {
        if (last_pageflip == -1)
        {
            last_pageflip = get_current_time();
            return;
        }

        const int64_t refresh = this->refresh_nsec / 1e6;
        const int64_t on_time_thresh = refresh * 1.5;
        const int64_t last_frame_len = get_current_time() - last_pageflip;
        if (last_frame_len <= on_time_thresh)
        {
            // We rendered last frame on time
            if (get_current_time() - last_increase >= increase_window)
            {
                increase_window = clamp(int64_t(increase_window * 0.75),
                    MIN_INCREASE_WINDOW, MAX_INCREASE_WINDOW);
                update_delay(+1);
                reset_increase_timer();

                // If we manage the next few frames, then we have reached a new
                // stable state
                expand_inc_window_on_miss = 20;
            } else
            {
                --expand_inc_window_on_miss;
            }

            // Stop exponential decrease
            consecutive_decrease = 1;
        } else
        {
            // We missed last frame.
            update_delay(-consecutive_decrease);
            // Next decrease should be faster
            consecutive_decrease = clamp(consecutive_decrease * 2, 1, 32);

            // Next increase should be tried after a longer interval
            if (expand_inc_window_on_miss >= 0)
            {
                increase_window = clamp(increase_window * 2,
                    MIN_INCREASE_WINDOW, MAX_INCREASE_WINDOW);
            }

            reset_increase_timer();
        }

        last_pageflip = get_current_time();
    }

    /**
     * @return The delay in milliseconds for the current frame.
     */
    int get_delay()
    {
        return delay;
    }

  private:
    int delay = 0;

    void update_delay(int delta)
    {
        int config_delay = std::max(0,
            (int)(this->refresh_nsec / 1e6) - max_render_time);

        int min = 0;
        int max = config_delay;
        if (max_render_time == -1)
        {
            max = 0;
        } else if (!dynamic_delay)
        {
            min = config_delay;
            max = config_delay;
        }

        delay = clamp(delay + delta, min, max);
    }

    void reset_increase_timer()
    {
        last_increase = get_current_time();
    }

    static constexpr int64_t MIN_INCREASE_WINDOW = 200; // 200 ms
    static constexpr int64_t MAX_INCREASE_WINDOW = 30'000; // 30s
    int64_t increase_window = MIN_INCREASE_WINDOW;
    int64_t last_increase   = 0;

    // > 0 => Increase increase_window
    int64_t expand_inc_window_on_miss = 0;

    // Expontential decrease in case of missed frames
    int32_t consecutive_decrease = 1;

    // Time of last frame
    int64_t last_pageflip = -1; // -1 is invalid

    int64_t refresh_nsec;
    wf::option_wrapper_t<int> max_render_time{"core/max_render_time"};
    wf::option_wrapper_t<bool> dynamic_delay{"workarounds/dynamic_repaint_delay"};

    wf::wl_listener_wrapper on_present;
};

class wf::render_manager::impl
{
  public:
    wf::wl_listener_wrapper on_frame;
    wf::wl_timer<false> repaint_timer;

    output_t *output;
    wf::region_t swap_damage;
    std::unique_ptr<swapchain_damage_manager_t> damage_manager;
    std::unique_ptr<effect_hook_manager_t> effects;
    std::unique_ptr<postprocessing_manager_t> postprocessing;
    std::unique_ptr<depth_buffer_manager_t> depth_buffer_manager;
    std::unique_ptr<repaint_delay_manager_t> delay_manager;

    wf::option_wrapper_t<wf::color_t> background_color_opt;
    std::unique_ptr<wf::render_pass_t> current_pass;
    wf::option_wrapper_t<std::string> icc_profile;

    wlr_color_transform *get_color_transform()
    {
        return icc_color_transform;
    }

    impl(output_t *o) : output(o), env_allow_scanout(check_scanout_enabled())
    {
        damage_manager = std::make_unique<swapchain_damage_manager_t>(o);
        effects = std::make_unique<effect_hook_manager_t>();
        postprocessing = std::make_unique<postprocessing_manager_t>(o);
        depth_buffer_manager = std::make_unique<depth_buffer_manager_t>();
        delay_manager = std::make_unique<repaint_delay_manager_t>(o);

        on_frame.set_callback([&] (void*)
        {
            /* If the session is not active, don't paint.
             * This is the case when e.g. switching to another tty */
            if (wf::get_core().session && !wf::get_core().session->active)
            {
                return;
            }

            delay_manager->start_frame();

            auto repaint_delay = delay_manager->get_delay();
            // Leave a bit of time for clients to render, see
            // https://github.com/swaywm/sway/pull/4588
            if (repaint_delay < 1)
            {
                output->handle->frame_pending = false;
                paint();
            } else
            {
                output->handle->frame_pending = true;
                repaint_timer.set_timeout(repaint_delay, [=] ()
                {
                    output->handle->frame_pending = false;
                    paint();
                });
            }

            frame_done_signal ev;
            output->emit(&ev);
        });

        on_frame.connect(&output->handle->events.frame);

        background_color_opt.load_option("core/background_color");
        background_color_opt.set_callback([=] ()
        {
            damage_manager->damage_whole_idle();
        });

        damage_manager->schedule_repaint();

        auto section = wf::get_core().config_backend->get_output_section(output->handle);
        icc_profile.load_option(section->get_name() + "/icc_profile");
        icc_profile.set_callback([=] ()
        {
            reload_icc_profile();
            damage_manager->damage_whole_idle();
        });

        reload_icc_profile();
    }

    wlr_color_transform *icc_color_transform = NULL;
    wlr_buffer_pass_options pass_opts;

    void reload_icc_profile()
    {
        if (icc_profile.value().empty())
        {
            set_icc_transform(nullptr);
            return;
        }

        if (!wf::get_core().is_vulkan())
        {
            LOGW("ICC profiles in core are only supported with the vulkan renderer. "
                 "For GLES2, make sure to enable the vk-color-management plugin.");
        }

        auto path = std::filesystem::path{icc_profile.value()};
        if (std::filesystem::is_regular_file(path))
        {
            // Read binary file into vector<char> buffer
            std::ifstream file(icc_profile.value(), std::ios::binary);
            std::vector<char> buffer((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());

            auto transform = wlr_color_transform_init_linear_to_icc(buffer.data(), buffer.size());
            if (!transform)
            {
                LOGE("Failed to load ICC transform from ", icc_profile.value());
                set_icc_transform(nullptr);
                return;
            } else
            {
                LOGI("Loaded ICC transform from ", icc_profile.value(), " for output ", output->to_string());
            }

            set_icc_transform(transform);
        }
    }

    void set_icc_transform(wlr_color_transform *transform)
    {
        if (icc_color_transform)
        {
            wlr_color_transform_unref(icc_color_transform);
        }

        icc_color_transform = transform;
    }

    ~impl()
    {
        set_icc_transform(nullptr);
    }

    const bool env_allow_scanout;
    static bool check_scanout_enabled()
    {
        const char *env_scanout = getenv("WAYFIRE_DISABLE_DIRECT_SCANOUT");
        bool env_allow_scanout  = (env_scanout == nullptr) || (!strcmp(env_scanout, "0"));
        if (!env_allow_scanout)
        {
            LOGC(SCANOUT, "Scanout disabled by environment variable.");
        }

        return env_allow_scanout;
    }

    int output_inhibit_counter = 0;
    void add_inhibit(bool add)
    {
        output_inhibit_counter += add ? 1 : -1;
        if (output_inhibit_counter == 0)
        {
            damage_manager->damage_whole_idle();

            wf::output_start_rendering_signal data;
            data.output = output;
            output->emit(&data);
        }
    }

    /* Actual rendering functions */

    /**
     * Try to directly scanout a view on the output, thereby skipping rendering
     * entirely.
     *
     * @return True if scanout was successful, False otherwise.
     */
    bool do_direct_scanout()
    {
        const bool can_scanout = !output_inhibit_counter && effects->can_scanout() &&
            postprocessing->can_scanout() && wlr_output_is_direct_scanout_allowed(output->handle) &&
            (icc_color_transform == nullptr);

        if (!can_scanout || !env_allow_scanout)
        {
            return false;
        }

        auto result = scene::try_scanout_from_list(
            damage_manager->render_instances, output);
        return result == scene::direct_scanout::SUCCESS;
    }

    /**
     * Return the swap damage if called from overlay or postprocessing
     * effect callbacks or empty region otherwise.
     */
    wf::region_t get_swap_damage()
    {
        return swap_damage;
    }

    /**
     * Render an output. Either calls the built-in renderer, or the render hook
     * of a plugin
     */
    wf::region_t start_output_pass(
        std::unique_ptr<swapchain_damage_manager_t::frame_object_t>& next_frame)
    {
        render_pass_params_t params;
        params.instances = &damage_manager->render_instances;
        params.damage    = damage_manager->get_ws_damage(
            output->wset()->get_current_workspace());
        params.damage += wf::origin(output->get_layout_geometry());

        params.target = postprocessing->get_target_framebuffer().translated(
            wf::origin(output->get_layout_geometry()));
        params.background_color = background_color_opt;
        params.reference_output = this->output;
        params.renderer = output->handle->renderer;
        params.flags    = RPASS_CLEAR_BACKGROUND | RPASS_EMIT_SIGNALS;

        pass_opts.timer = NULL; // TODO: do we care about this? could be useful for dynamic frame scheduling
        pass_opts.color_transform = icc_color_transform;
        params.pass_opts   = &pass_opts;
        this->current_pass = std::make_unique<render_pass_t>(params);
        auto total_damage = current_pass->run_partial();

        total_damage += -wf::origin(output->get_layout_geometry());
        total_damage  = total_damage * output->handle->scale;
        total_damage &= damage_manager->get_wlr_damage_box();

        if (runtime_config.damage_debug)
        {
            /* Clear the screen to yellow, so that the repainted parts are visible */
            wf::region_t yellow = damage_manager->get_wlr_damage_box();
            yellow ^= total_damage;

            total_damage |= damage_manager->get_wlr_damage_box();
            current_pass->clear(yellow, {1, 1, 0, 1});
        }

        return total_damage;
    }

    void update_bound_output(wlr_buffer *buffer)
    {
        /* Make sure the default buffer has enough size */
        postprocessing->allocate(output->handle->width, output->handle->height);
        postprocessing->set_current_buffer(buffer);

        if (wf::get_core().is_gles2())
        {
            const auto& default_fb = postprocessing->get_target_framebuffer();
            GLuint default_fb_id   = gles::ensure_render_buffer_fb_id(default_fb);
            depth_buffer_manager->ensure_depth_buffer(default_fb_id,
                default_fb.get_size().width, default_fb.get_size().height);
        }
    }

    /**
     * Repaints the whole output, includes all effects and hooks
     */
    void paint()
    {
        /* Part 1: frame setup: query damage, etc. */
        effects->run_effects(OUTPUT_EFFECT_PRE);
        effects->run_effects(OUTPUT_EFFECT_DAMAGE);

        if (do_direct_scanout())
        {
            // Yet another optimization: if we can directly scanout, we should
            // stop the rest of the repaint cycle.
            return;
        }

        auto next_frame = damage_manager->start_frame();
        if (!next_frame)
        {
            // Optimization: the output doesn't need a new frame (so isn't damaged), so we can
            // just skip the whole repaint
            delay_manager->skip_frame();
            return;
        }

        /* Part 2: call the renderer, which sets swap_damage and draws the scenegraph */
        update_bound_output(next_frame->buffer);
        this->swap_damage = start_output_pass(next_frame);

        /* Part 3: overlay effects */
        effects->run_effects(OUTPUT_EFFECT_OVERLAY);

        /* Part 4: finalize the scene: postprocessing effects */
        if (postprocessing->post_effects.size())
        {
            swap_damage |= damage_manager->get_wlr_damage_box();
        }

        postprocessing->run_post_effects();
        if (output_inhibit_counter)
        {
            current_pass->clear(current_pass->get_target().geometry, {0, 0, 0, 1});
        }

        /* Part 5: render sw cursors
         * We render software cursors after everything else
         * for consistency with hardware cursor planes */
        wlr_output_add_software_cursors_to_render_pass(output->handle, current_pass->get_wlr_pass(),
            swap_damage.to_pixman());

        /* Part 6: finalize frame: swap buffers, send frame_done, etc */
        damage_manager->swap_buffers(std::move(next_frame), std::move(*current_pass.release()), swap_damage);

        postprocessing->set_current_buffer(nullptr);

        swap_damage.clear();
        post_paint();
    }

    /**
     * Execute post-paint actions.
     */
    void post_paint()
    {
        effects->run_effects(OUTPUT_EFFECT_POST);
        if (damage_manager->constant_redraw_counter)
        {
            damage_manager->schedule_repaint();
        }
    }
};

scene::direct_scanout scene::try_scanout_from_list(
    const std::vector<scene::render_instance_uptr>& instances,
    wf::output_t *scanout)
{
    for (auto& ch : instances)
    {
        auto res = ch->try_scanout(scanout);
        if (res != direct_scanout::SKIP)
        {
            return res;
        }
    }

    return direct_scanout::SKIP;
}

void scene::compute_visibility_from_list(const std::vector<render_instance_uptr>& instances,
    wf::output_t *output, wf::region_t& region, const wf::point_t& offset)
{
    region -= offset;
    for (auto& ch : instances)
    {
        ch->compute_visibility(output, region);
    }

    region += offset;
}

render_manager::render_manager(output_t *o) :
    pimpl(new impl(o))
{}
render_manager::~render_manager() = default;

void render_manager::set_redraw_always(bool always)
{
    pimpl->damage_manager->set_redraw_always(always);
}

wf::region_t render_manager::get_swap_damage()
{
    return pimpl->get_swap_damage();
}

void render_manager::schedule_redraw()
{
    pimpl->damage_manager->schedule_repaint();
}

void render_manager::add_inhibit(bool add)
{
    pimpl->add_inhibit(add);
}

void render_manager::add_effect(effect_hook_t *hook, output_effect_type_t type)
{
    pimpl->effects->add_effect(hook, type);
}

void render_manager::rem_effect(effect_hook_t *hook)
{
    pimpl->effects->rem_effect(hook);
}

void render_manager::add_post(post_hook_t *hook)
{
    pimpl->postprocessing->add_post(hook);
}

void render_manager::rem_post(post_hook_t *hook)
{
    pimpl->postprocessing->rem_post(hook);
}

wf::region_t render_manager::get_scheduled_damage()
{
    return pimpl->damage_manager->get_scheduled_damage();
}

void render_manager::damage_whole()
{
    pimpl->damage_manager->damage_whole();
}

void render_manager::damage_whole_idle()
{
    pimpl->damage_manager->damage_whole_idle();
}

void render_manager::damage(const wlr_box& box, bool repaint)
{
    pimpl->damage_manager->damage(box, repaint);
}

void render_manager::damage(const wf::region_t& region, bool repaint)
{
    pimpl->damage_manager->damage(region, repaint);
}

wlr_box render_manager::get_ws_box(wf::point_t ws) const
{
    return pimpl->damage_manager->get_ws_box(ws);
}

wlr_color_transform*render_manager::get_color_transform()
{
    return pimpl->get_color_transform();
}

wf::render_target_t render_manager::get_target_framebuffer() const
{
    return pimpl->postprocessing->get_target_framebuffer();
}

void render_manager::set_require_depth_buffer(bool require)
{
    return pimpl->depth_buffer_manager->set_required(require);
}

wf::render_pass_t*render_manager::get_current_pass()
{
    return pimpl->current_pass.get();
}

void priv_render_manager_clear_instances(wf::render_manager *manager)
{
    manager->pimpl->damage_manager->render_instances.clear();
    manager->pimpl->damage_manager->root_update.disconnect();
}

void priv_render_manager_start_rendering(wf::render_manager *manager)
{
    manager->pimpl->damage_manager->start_rendering();
}
} // namespace wf

/* End render_manager */
