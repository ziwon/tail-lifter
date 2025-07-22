# Local Testing Guide

This document outlines the steps to build the necessary components, set up a local Kubernetes cluster using KIND, deploy `tail-lifter`, and verify its end-to-end functionality.

### **Phase 1: Prerequisites & Initial Setup**

1.  **Install Tooling**: Ensure the following CLI tools are installed on your local machine:
    *   `just`
    *   `docker` (or a compatible container runtime)
    *   `kind`
    *   `kubectl`
    *   `helm`
    *   `tailscale`

2.  **Authenticate Tailscale**: Authenticate the Tailscale client on your host machine and ensure it's connected to your tailnet.
    ```bash
    sudo tailscale up
    ```

3.  **Clone the Repository**: You should already have the project code locally.

### **Phase 2: Build and Deploy**

1.  **Build the eBPF Program**: Compile the C code into a BPF object file.
    ```bash
    just build
    ```

2.  **Spin up the KIND Cluster**: Create a new cluster with the configuration specified in `deploy/kind.yaml`. This command also installs Cilium as the CNI.
    ```bash
    just kind-up
    ```

3.  **Build the Controller Image**: Build the main `tail-lifter` container image.
    ```bash
    just image-build
    ```

4.  **Load the Image into KIND**: Make the newly built image available to the KIND cluster nodes.
    ```bash
    kind load docker-image ghcr.io/ziwon/tail-lifter:latest --name tail-lifter-dev
    ```

5.  **Deploy `tail-lifter`**: Apply the necessary RBAC permissions and deploy the `tail-lifter` DaemonSet to the `kube-system` namespace.
    ```bash
    kubectl apply -f deploy/rbac.yaml
    just deploy
    ```

### **Phase 3: Deploy a Test Application**

1.  **Create a Test Namespace**:
    ```bash
    kubectl create ns tail-lifter-test
    ```

2.  **Deploy a Sample Application**: Deploy a simple `nginx` web server to act as the backend service.
    ```bash
    kubectl apply -n tail-lifter-test -f - <<EOF
    apiVersion: apps/v1
    kind: Deployment
    metadata:
      name: nginx-deployment
    spec:
      replicas: 1
      selector:
        matchLabels:
          app: nginx
      template:
        metadata:
          labels:
            app: nginx
        spec:
          containers:
          - name: nginx
            image: nginx:latest
            ports:
            - containerPort: 80
    EOF
    ```

3.  **Expose the Application with a Service**: Create a ClusterIP service for the `nginx` deployment.
    ```bash
    kubectl apply -n tail-lifter-test -f - <<EOF
    apiVersion: v1
    kind: Service
    metadata:
      name: nginx-service
    spec:
      selector:
        app: nginx
      ports:
        - protocol: TCP
          port: 80
          targetPort: 80
    EOF
    ```

### **Phase 4: Verification**

1.  **Identify the Service IP**: The controller deterministically assigns a ClusterIP. Based on the logic in `main.go`, the IP for our test service (`nginx-service` in `tail-lifter-test` namespace) will be `10.8.43.159`.

2.  **Advertise Route**: On your host machine, advertise this specific IP address to your tailnet.
    ```bash
    sudo tailscale up --advertise-routes=10.8.43.159/32
    ```
    After running this command, you must **go to the Tailscale admin console** and approve the newly advertised route.

3.  **Run the Test**: From your host machine (or any other device on your tailnet), attempt to connect to the `nginx` service using the advertised IP.
    ```bash
    curl http://10.8.43.159
    ```

4.  **Confirm the Result**: The command should return the default "Welcome to nginx!" HTML page. This confirms that `tail-lifter` has successfully performed DNAT to route the request to the pod and SNAT on the return path.

5.  **Check Logs (Optional)**: You can inspect the logs of the `tail-lifter` pods to see the controller logic in action.
    ```bash
    kubectl logs -n kube-system -l app=tail-lifter --tail=-1
    ```

### **Phase 5: Cleanup**

1.  **Destroy the KIND Cluster**:
    ```bash
    just kind-down
    ```

2.  **Clean Build Artifacts**:
    ```bash
    just clean
    ```

3.  **Disable Route**: Go to your Tailscale admin console and disable or remove the `10.8.43.159/32` route.
