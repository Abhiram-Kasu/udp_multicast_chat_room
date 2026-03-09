use dioxus::prelude::*;

/// Returns a CSS class name for the room avatar color based on the room ID.
pub fn avatar_color_class(id: u64) -> String {
    format!("color-{}", id % 8)
}

/// Returns 1–2 character initials from a room name for the avatar.
pub fn room_initials(name: &str) -> String {
    let words: Vec<&str> = name.split_whitespace().collect();
    match words.len() {
        0 => "#".to_string(),
        1 => words[0].chars().take(2).collect::<String>().to_uppercase(),
        _ => {
            let first = words[0].chars().next().unwrap_or('#');
            let second = words[1].chars().next().unwrap_or('#');
            format!("{}{}", first, second).to_uppercase()
        }
    }
}

/// Returns a simple HH:MM time string from the current system time (UTC).
pub fn current_time_str() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let hours = (now / 3600) % 24;
    let minutes = (now / 60) % 60;
    format!("{:02}:{:02}", hours, minutes)
}

/// Pushes a toast notification into the shared toast signal and schedules
/// its automatic removal after 4 seconds.
pub fn add_toast(
    toasts: &mut Signal<Vec<(String, String, u64)>>,
    toast_counter: &mut Signal<u64>,
    kind: &str,
    message: String,
) {
    let id = (*toast_counter)();
    *toast_counter.write() += 1;
    toasts.write().push((kind.to_string(), message, id));
    // Auto-remove toast after 4 seconds
    let mut toasts_clone = *toasts;
    dioxus::prelude::spawn(async move {
        tokio::time::sleep(tokio::time::Duration::from_secs(4)).await;
        toasts_clone.write().retain(|(_, _, tid)| *tid != id);
    });
}
