mod control_socket;
mod packets;
mod stream_socket;

use alvr_common::prelude::*;
use rand::Rng;
use serde::{Deserialize, Serialize};
use std::net::{IpAddr, Ipv4Addr};

pub use control_socket::*;
pub use packets::*;
pub use stream_socket::*;

pub const LOCAL_IP: IpAddr = IpAddr::V4(Ipv4Addr::UNSPECIFIED);
pub const CONTROL_PORT: u16 = 9943;
pub const MAX_HANDSHAKE_PACKET_SIZE_BYTES: usize = 4_000;

type Ldc = tokio_util::codec::LengthDelimitedCodec;

#[derive(Serialize, Deserialize, Clone)]
pub struct PublicIdentity {
    pub hostname: String,
    pub certificate_pem: Option<String>,
}

pub struct PrivateIdentity {
    pub hostname: String,
    pub certificate_pem: String,
    pub key_pem: String,
}

pub fn create_identity(hostname: Option<String>) -> StrResult<PrivateIdentity> {
    let hostname = hostname.unwrap_or(format!(
        "{}{}{}{}.client.alvr",
        rand::thread_rng().gen_range(0..10),
        rand::thread_rng().gen_range(0..10),
        rand::thread_rng().gen_range(0..10),
        rand::thread_rng().gen_range(0..10),
    ));

    #[cfg(target_os = "android")]
    let certificate = trace_err!(rcgen::generate_simple_self_signed([hostname.clone()]))?;

    #[cfg(not(target_os = "android"))]
    return Ok(PrivateIdentity {
        hostname,
        certificate_pem: String::new(),
        key_pem: String::new(),
    });

    #[cfg(target_os = "android")]
    return Ok(PrivateIdentity {
        hostname,
        certificate_pem: trace_err!(certificate.serialize_pem())?,
        key_pem: certificate.serialize_private_key_pem(),
    });
}

mod util {
    use alvr_common::prelude::*;
    use std::future::Future;
    use tokio::{sync::oneshot, task};

    // Tokio tasks are not cancelable. This function awaits a cancelable task.
    pub async fn spawn_cancelable(
        future: impl Future<Output = StrResult> + Send + 'static,
    ) -> StrResult {
        // this channel is actually never used. cancel_receiver will be notified when _cancel_sender
        // is dropped
        let (_cancel_sender, cancel_receiver) = oneshot::channel::<()>();

        trace_err!(
            task::spawn(async {
                tokio::select! {
                    res = future => res,
                    _ = cancel_receiver => Ok(()),
                }
            })
            .await
        )?
    }
}
pub use util::*;
