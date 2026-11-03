//! HTTPS client for the host's paired API (port 47984 by default).
//!
//! Hosts use self-signed certificates, so trust comes from pairing: the certificate the host sent
//! during pairing is stored and every later HTTPS connection must present exactly that
//! certificate. The client authenticates with its own certificate (mutual TLS).

use anyhow::{Context, Result};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls12_signature, verify_tls13_signature, CryptoProvider};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, Error as TlsError, SignatureScheme};
use std::sync::Arc;
use std::time::Duration;

use crate::identity::ClientIdentity;

/// What the server certificate must match.
#[derive(Debug, Clone)]
pub enum ServerTrust {
    /// The DER certificate stored at pairing time.
    Pinned(Vec<u8>),
}

#[derive(Debug)]
struct Verifier {
    trust: ServerTrust,
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for Verifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, TlsError> {
        match &self.trust {
            ServerTrust::Pinned(expected) if expected.as_slice() == end_entity.as_ref() => {
                Ok(ServerCertVerified::assertion())
            }
            ServerTrust::Pinned(_) => Err(TlsError::General(
                "host certificate differs from the one pinned at pairing".into(),
            )),
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

/// Builds a mutual-TLS HTTP client. `timeout` bounds each whole request; launching can take
/// a long time on some hosts, so callers pick it per operation.
pub fn https_client(
    identity: &ClientIdentity,
    trust: ServerTrust,
    timeout: Duration,
) -> Result<reqwest::Client> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
        pem::parse(&identity.key_pem)
            .context("parsing client key PEM")?
            .into_contents(),
    ));
    let config = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Verifier { trust, provider }))
        .with_client_auth_cert(vec![CertificateDer::from(identity.cert_der.clone())], key)
        .context("configuring client certificate")?;
    Ok(reqwest::Client::builder()
        .use_preconfigured_tls(config)
        .connect_timeout(Duration::from_secs(10))
        .timeout(timeout)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .build()?)
}

/// Plain HTTP client for the unpaired API (port 47989 by default).
pub fn http_client(timeout: Duration) -> Result<reqwest::Client> {
    Ok(reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(10))
        .timeout(timeout)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .build()?)
}

/// HTTPS API with an explicit leaf-certificate pin and no client identity (Sunshine PIN UI).
pub fn pinned_server_client(
    certificate_der: Vec<u8>,
    timeout: Duration,
) -> Result<reqwest::Client> {
    crate::identity::certificate_signature(&certificate_der)?;
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let config = rustls::ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Verifier {
            trust: ServerTrust::Pinned(certificate_der),
            provider,
        }))
        .with_no_client_auth();
    Ok(reqwest::Client::builder()
        .use_preconfigured_tls(config)
        .connect_timeout(Duration::from_secs(10))
        .timeout(timeout)
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .build()?)
}
