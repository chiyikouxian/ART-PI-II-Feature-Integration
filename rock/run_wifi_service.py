import os
import sys


def main() -> None:
    here = os.path.dirname(os.path.abspath(__file__))
    svc_dir = os.path.join(here, "wifi_service")
    sys.path.insert(0, svc_dir)

    # Ensure config resolves inside the deploy folder by default.
    os.environ.setdefault("WIFI_SERVICE_CONFIG", os.path.join(svc_dir, "service_config.json"))

    import service_main  # type: ignore

    service_main.main()


if __name__ == "__main__":
    main()
