/* Shared authentication route assembly; included inside server startup. */
    register_uri(s_server, "/api/auth/setup", HTTP_GET, auth_setup_handler, false);
    register_uri(s_server, "/api/auth/setup", HTTP_POST, auth_setup_handler, false);
    register_uri(s_server, "/api/auth/certificate", HTTP_GET, auth_certificate_handler, false);
    register_uri(s_server, "/api/auth/login", HTTP_POST, auth_login_handler, false);
    register_uri(s_server, "/api/auth/login/status", HTTP_GET, auth_login_status_handler, false);
    register_uri(s_server, "/api/auth/logout", HTTP_POST, auth_logout_handler, false);
    register_uri(s_server, "/api/auth/status", HTTP_GET, auth_status_handler, false);
    register_uri(s_server, "/api/auth/two-step", HTTP_GET, two_step_handler, false);
    register_uri(s_server, "/api/auth/two-step", HTTP_POST, two_step_handler, false);
    register_uri(s_server, "/api/auth/passkeys", HTTP_GET, passkey_handler, false);
    register_uri(s_server, "/api/auth/passkeys", HTTP_POST, passkey_handler, false);
