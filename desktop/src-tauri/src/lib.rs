pub mod commands;
pub mod dto;
pub mod media;
pub mod storage;
pub mod vlm;
pub mod worker;

use std::sync::Arc;

use tauri::Manager;

use commands::AppState;
use media::{register_sublift_protocol, MediaStreamManager};
use storage::{CheckpointStore, ProviderStore, SessionStore, StorageRoot};
use vlm::NativeVlmService;
use worker::{WorkerBackend, WorkerConfig, WorkerSupervisor};

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let media_manager = Arc::new(MediaStreamManager::new());

    let builder = tauri::Builder::default().plugin(tauri_plugin_dialog::init());
    let builder = register_sublift_protocol(builder, Arc::clone(&media_manager));

    let app = builder
        .setup(move |app| {
            let storage = StorageRoot::from_tauri_app(app.handle())?;
            let resource_dir = app.path().resource_dir().ok();
            let worker = Arc::new(WorkerSupervisor::new(WorkerConfig {
                override_bin: None,
                resource_dir,
                repo_root: None,
            }));
            let providers = Arc::new(ProviderStore::new(storage.clone()));
            let sessions = Arc::new(SessionStore::new(storage.clone()));
            let checkpoints = Arc::new(CheckpointStore::new(storage.clone()));
            let vlm = Arc::new(NativeVlmService::new(
                Arc::clone(&worker) as Arc<dyn WorkerBackend>,
                Arc::clone(&providers),
                checkpoints,
                storage.clone(),
            ));
            let state = AppState::with_stores(
                worker,
                vlm,
                storage,
                providers,
                sessions,
                Arc::clone(&media_manager),
            );
            app.manage(state);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::get_capabilities,
            commands::probe_video,
            commands::extract_preview_frame,
            commands::detect_auto_region,
            commands::start_extraction,
            commands::cancel_extraction,
            commands::re_recognize_entries,
            commands::export_srt,
            commands::batch_export_srt,
            commands::pick_video_files,
            commands::pick_save_srt_path,
            commands::list_providers,
            commands::save_provider,
            commands::test_provider_connection,
            commands::delete_provider,
            commands::import_providers_from_env,
            commands::load_session_draft,
            commands::save_session_draft,
            commands::list_recent_sessions,
            commands::stop_media_stream,
        ])
        .build(tauri::generate_context!())
        .expect("error while building SubLift desktop application");

    app.run(|app_handle, event| {
        if let tauri::RunEvent::Exit = event {
            if let Some(state) = app_handle.try_state::<AppState>() {
                tauri::async_runtime::block_on(async {
                    state.worker.shutdown().await;
                    state.media.stop_all().await;
                });
            }
        }
    });
}
