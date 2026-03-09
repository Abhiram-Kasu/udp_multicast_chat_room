use dioxus::prelude::*;

#[component]
pub fn ToastContainer() -> Element {
    let toasts = use_context::<Signal<Vec<(String, String, u64)>>>();
    let items = toasts();

    rsx! {
        div { class: "toast-container",
            for (kind, message, id) in items.iter() {
                div { class: "toast {kind}", key: "{id}",
                    span { class: "toast-icon",
                        if kind == "error" {
                            "⚠"
                        } else if kind == "success" {
                            "✓"
                        } else {
                            "ℹ"
                        }
                    }
                    span { "{message}" }
                }
            }
        }
    }
}
