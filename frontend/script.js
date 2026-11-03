async function loadVehicles() {
  const res = await fetch('/api/vehicles');
  const data = await res.json();
  const tbody = document.querySelector('#vehicleTable tbody');
  tbody.innerHTML = '';
  data.forEach(v => {
    tbody.innerHTML += `<tr>
      <td>${v.id}</td><td>${v.type}</td><td>${v.brand}</td><td>${v.model}</td>
      <td>₹${v.ratePerDay}</td>
      <td>${v.available ? 'Available' : 'Rented'}</td></tr>`;
  });
}

async function book() {
  const body = {
    vehicleId: +document.getElementById('vid').value,
    customer: document.getElementById('cname').value,
    days: +document.getElementById('days').value
  };
  const res = await fetch('/api/book', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)
  });
  const data = await res.json();
  document.getElementById('msg').innerText =
    res.ok ? `Booked! Booking ID: ${data.bookingId}, Amount: ₹${data.amount}` : data.error;
  loadVehicles();
}

loadVehicles();